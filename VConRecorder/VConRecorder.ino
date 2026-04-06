
// =============================================================================
// M5Stack Core2 — vCon Audio Recorder  (VConic)
// =============================================================================
// Records audio from the built-in microphone and POSTs a spec-compliant
// vCon JSON (IETF draft-ietf-vcon-vcon-core) to a configured endpoint.
// Audio is embedded inline as base64url-encoded WAV.
//
// Serial Commands (115200 baud):
//   wifi <ssid> <password>  — Set WiFi credentials (saved to flash)
//   url  <post_url>         — Set vCon POST URL   (saved to flash)
//   status                  — Show current configuration
//   restart                 — Restart device
//   help                    — Show available commands
//
// Capacitive touch buttons (below screen):
//   Left  (A) = RUN     Start recording, then auto-encode + upload
//   Center(B) = STOP    Stop recording early (encodes + uploads partial audio)
//   Right (C) = CONFIG  Show configuration screen
// =============================================================================

#include <M5Unified.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <time.h>
#include "mbedtls/base64.h"
#include "esp_system.h"
#include "esp_mac.h"
#include <SD.h>
#include "config.h"
#include "logo.h"
#include "ota.h"

// =============================================================================
// State Machine
// =============================================================================

enum AppState {
    STATE_IDLE,
    STATE_RECORDING,
    STATE_ENCODING,
    STATE_UPLOADING,
    STATE_SUCCESS,
    STATE_ERROR,
    STATE_CONTINUOUS    // continuous record+post loop
};

// =============================================================================
// Brand colour — VConic green replaces generic TFT_YELLOW for panel chrome
// =============================================================================
// 0x3666 ≈ RGB(49, 204, 49)  — a clean mid-bright green that reads well at
// text size 1 with black foreground, and echoes the VConic logo palette.
#define VC_GREEN 0x3666u

// UI layout — persistent chrome heights
#define UI_TOP_H     8    // hazard stripe
#define UI_CONTENT_Y 8    // content area starts here
#define UI_CONTENT_H 186  // 240 - 8 - 46
#define UI_BTN_Y     194  // button row y
#define UI_BTN_H     46   // button row height

// =============================================================================
// Globals
// =============================================================================

// Config
Preferences prefs;
String wifiSSID, wifiPassword, postURL, deviceToken;
uint32_t recordDurationSec = RECORD_DURATION_SEC;  // runtime-settable via 'dur' command

// Device unique identifier — "VC-XXXXXX" derived from last 3 MAC bytes.
// Set once in setup(), before the splash screen.
String deviceID = "";

// WiFi
bool wifiConnected = false;
int  wifiRSSI      = 0;
bool ntpSynced     = false;
unsigned long lastWifiCheckMs = 0;

// State
AppState appState   = STATE_IDLE;
String   lastStatus = "Press RECORD to begin";
int      lastHttpCode  = 0;
String   lastVconUUID  = "--";
uint32_t totalUploads  = 0;
uint32_t failedUploads = 0;
unsigned long stateChangeMs = 0;  // for auto-return from SUCCESS/ERROR

// Audio buffer (PSRAM)
int16_t* audioBuffer      = nullptr;
size_t   audioBufferIndex = 0;   // samples filled so far
bool     isRecording      = false;
unsigned long recordStartMs = 0;

// =============================================================================
// Continuous mode — dual-buffer double-buffered record+upload
// =============================================================================
// Two PSRAM buffers of equal size (AUDIO_PCM_BYTES each, ~240 KB at 15 s).
// While one buffer is being recorded into (recordBuf), the other is passed to
// a background FreeRTOS task on core 0 for encoding and HTTP POST (uploadBuf).
// The main loop on core 1 handles mic DMA, display, and button events.
//
// Thread-safety: the upload task ONLY writes to the volatile primitives below.
// All String/display operations stay on core 1.
// =============================================================================

// Two PSRAM audio buffers; audioBufA == audioBuffer (reused allocation)
int16_t* audioBufA  = nullptr;
int16_t* audioBufB  = nullptr;
int16_t* recordBuf  = nullptr;   // which buffer is currently being recorded into

// Continuous mode flags (volatile — written from upload task on core 0)
volatile bool continuousMode = false;
volatile bool stopContinuous = false;
volatile bool uploadBusy     = false;

// Upload task state reported back to main loop (all volatile, no Strings)
enum UploadTaskState : uint8_t {
    UTS_IDLE = 0, UTS_ENCODING, UTS_SD, UTS_UPLOADING, UTS_RETRY,
    UTS_DONE_OK, UTS_DONE_FAIL
};
volatile UploadTaskState uploadTaskState = UTS_IDLE;
volatile int32_t  uploadTaskHttpCode  = 0;
volatile uint8_t  uploadRetryNum      = 0;
volatile uint32_t continuousChunks    = 0;   // how many chunks successfully posted

// Parameters passed to the upload task
struct UploadJob {
    int16_t* buf;
    size_t   numSamples;
};
static UploadJob g_uploadJob;
TaskHandle_t     g_uploadTaskHandle = nullptr;

// Audio level history — 16 bars for the VU-meter display
#define LEVEL_BARS 16
int      levelHistory[LEVEL_BARS] = {0};
int      levelIdx = 0;

// Display
unsigned long lastDisplayMs = 0;
const unsigned long DISPLAY_INTERVAL_MS = 300;

// =============================================================================
// UI Screen Navigation
// =============================================================================

enum UIScreen : uint8_t {
    SCREEN_HOME     = 0,
    SCREEN_STATUS   = 1,
    SCREEN_CONFIG   = 2,
    SCREEN_TOOLS    = 3,
    SCREEN_DURATION = 4,  // interactive duration picker
    SCREEN_WIFI     = 5   // WiFi network picker
};

UIScreen currentScreen = SCREEN_HOME;

// Inactivity timer — return to HOME after 60 s without a button press
unsigned long lastButtonMs           = 0;
const unsigned long INACTIVITY_TIMEOUT_MS = 60000UL;

// Duration picker — scratch value while on SCREEN_DURATION.
// Committed to recordDurationSec only when the user presses SAVE.
uint32_t durEditPending = 0;

// TOOLS screen state machine
enum ToolsItem : uint8_t {
    TOOL_WIFI_TEST = 0,
    TOOL_OTA_TEST  = 1,
    TOOL_POST_TEST = 2,
    TOOL_SD_SHOW   = 3,
    TOOL_RESTART   = 4,
    TOOL_COUNT     = 5
};

enum ToolsPhase : uint8_t {
    TPHASE_SELECT  = 0,
    TPHASE_CONFIRM = 1,
    TPHASE_RUNNING = 2,
    TPHASE_RESULT  = 3
};

uint8_t    toolsSelectedItem = 0;
ToolsPhase toolsPhase        = TPHASE_SELECT;
String     toolsResultLine1  = "";
String     toolsResultLine2  = "";
bool       toolsResultOK     = false;
unsigned long toolsResultMs  = 0;
const unsigned long TOOLS_RESULT_TIMEOUT_MS = 8000UL;

// =============================================================================
// WiFi picker state
// =============================================================================

enum WifiPhase : uint8_t {
    WPHASE_SCANNING   = 0,
    WPHASE_SELECT     = 1,
    WPHASE_PASSWORD   = 2,
    WPHASE_CONNECTING = 3,
    WPHASE_RESULT     = 4
};

WifiPhase    wifiPhase        = WPHASE_SCANNING;

// Scan results — kept in parallel arrays to avoid dynamic allocation
#define WIFI_MAX_SCAN  20
int          wifiScanCount    = 0;
char         wifiScanSSID[WIFI_MAX_SCAN][33]; // 32-byte SSID max + null
int32_t      wifiScanRSSI[WIFI_MAX_SCAN];
bool         wifiScanOpen[WIFI_MAX_SCAN];   // true = no password required
uint8_t      wifiSelIdx       = 0;          // highlighted network in list
uint8_t      wifiScrollTop    = 0;          // first visible index in list

// Password character picker
static const char WIFI_CHARS[] =
    "abcdefghijklmnopqrstuvwxyz"
    "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
    "0123456789"
    " !@#$%^&*()-_=+[]{}|;:',.<>?/~";
#define WIFI_CHAR_COUNT  (sizeof(WIFI_CHARS) - 1)   // exclude null terminator
// Special virtual indices after the character set
#define WIFI_IDX_DONE   (WIFI_CHAR_COUNT)
#define WIFI_IDX_DEL    (WIFI_CHAR_COUNT + 1)
#define WIFI_IDX_TOTAL  (WIFI_CHAR_COUNT + 2)

String       wifiPwdBuf       = "";         // password being composed
uint16_t     wifiCharIdx      = 0;          // index into WIFI_CHARS + specials
unsigned long wifiResultMs    = 0;
bool         wifiResultOK     = false;
String       wifiResultMsg    = "";
bool         wifiWasConnected = false;      // WiFi state captured on picker entry

// Wifi retry
const unsigned long WIFI_RETRY_MS = 30000;

// SD card
bool     sdReady    = false;
uint32_t sdSaved    = 0;       // number of recordings saved to SD
String   sdCardInfo = "No card";

// Off-screen sprite for the MICROPHONE INPUT panel — eliminates SPI flicker
// by compositing the entire 160×87 px panel in RAM and pushing it as one burst.
M5Canvas audioSprite(&M5.Display);

// =============================================================================
// Runtime audio-size helpers
// =============================================================================
// Buffer allocation and fill limits are based on the current recordDurationSec,
// which is set at runtime and persisted to flash.  All code uses these helpers
// instead of compile-time constants so they pick up the live value.

inline uint32_t audioSampleTarget() {
    return recordDurationSec * (uint32_t)SAMPLE_RATE;
}
inline uint32_t audioPcmBytes() {
    return audioSampleTarget() * 2u;
}

// =============================================================================
// Configuration (Preferences / flash)
// =============================================================================

void loadConfig() {
    prefs.begin("vconrec", false);
    wifiSSID          = prefs.getString("wifi_ssid",  DEFAULT_WIFI_SSID);
    wifiPassword      = prefs.getString("wifi_pass",  DEFAULT_WIFI_PASSWORD);
    postURL           = prefs.getString("post_url",   DEFAULT_POST_URL);
    deviceToken       = prefs.getString("dev_token",  DEFAULT_DEVICE_TOKEN);
    totalUploads      = prefs.getUInt("total_up", 0);
    failedUploads     = prefs.getUInt("fail_up",  0);
    recordDurationSec = prefs.getUInt("rec_dur",  RECORD_DURATION_SEC);
    prefs.end();
    // Clamp in case a previously stored value is out of range
    if (recordDurationSec < MIN_RECORD_DURATION_SEC) recordDurationSec = MIN_RECORD_DURATION_SEC;
    if (recordDurationSec > MAX_RECORD_DURATION_SEC) recordDurationSec = MAX_RECORD_DURATION_SEC;
    Serial.printf("[config] SSID=%s\n[config] URL=%s\n",
                  wifiSSID.c_str(), postURL.c_str());
    Serial.printf("[config] Duration=%u s\n", recordDurationSec);
    if (deviceToken.length() > 0)
        Serial.printf("[config] Token=%s\n", deviceToken.c_str());
}

void saveConfig() {
    prefs.begin("vconrec", false);
    prefs.putString("wifi_ssid", wifiSSID);
    prefs.putString("wifi_pass", wifiPassword);
    prefs.putString("post_url",  postURL);
    prefs.putString("dev_token", deviceToken);
    prefs.putUInt("total_up",  totalUploads);
    prefs.putUInt("fail_up",   failedUploads);
    prefs.putUInt("rec_dur",   recordDurationSec);
    prefs.end();
}

// =============================================================================
// WiFi
// =============================================================================

void connectWiFi() {
    Serial.printf("[wifi] Connecting to: %s\n", wifiSSID.c_str());
    lastStatus = "Connecting WiFi...";
    // Fully reset the WiFi stack before each attempt; without this the ESP32
    // returns "sta is connecting, cannot set config" on subsequent calls.
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    delay(200);
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(false);
    delay(100);
    WiFi.begin(wifiSSID.c_str(), wifiPassword.c_str());
    for (int i = 0; i < 30 && WiFi.status() != WL_CONNECTED; i++) {
        delay(500);
        M5.update();
    }
    wifiConnected = (WiFi.status() == WL_CONNECTED);
    if (wifiConnected) {
        wifiRSSI = WiFi.RSSI();
        Serial.printf("[wifi] Connected — IP:%s RSSI:%d\n",
                      WiFi.localIP().toString().c_str(), wifiRSSI);
        lastStatus = "WiFi connected";
    } else {
        Serial.println("[wifi] Connection FAILED");
        lastStatus = "WiFi failed — use 'wifi' command";
    }
}

void checkWiFi() {
    if (WiFi.status() == WL_CONNECTED) {
        wifiConnected = true;
        wifiRSSI = WiFi.RSSI();
        return;
    }
    wifiConnected = false;
    unsigned long now = millis();
    if (now - lastWifiCheckMs >= WIFI_RETRY_MS) {
        lastWifiCheckMs = now;
        connectWiFi();
    }
}

// =============================================================================
// SD Card — time-bucketed storage with automatic rotation
// =============================================================================
//
// Files are stored under:
//   /wav/<YYYY>/<MM>/<DD>/<HH>/<uuid>.wav
//   /vcons/<YYYY>/<MM>/<DD>/<HH>/<uuid>.json
//
// Before each save the free space is checked.  If it falls below
// SD_MIN_FREE_BYTES (20 MB) the oldest hour-directory under /wav and /vcons
// is deleted to make room, repeating until enough space is available.
// =============================================================================

#define SD_MIN_FREE_BYTES (20ULL * 1024ULL * 1024ULL)   // 20 MB trigger threshold

void initSD() {
    // M5Unified uses LovyanGFX's own SPI driver for the display; it does NOT
    // initialise the Arduino SPI object. We must call SPI.begin() ourselves
    // with the Core2's VSPI pins before passing that bus to SD.begin().
    //
    // Core2 VSPI: CLK=18, MISO=38, MOSI=23, SD-CS=4
    SPI.begin(18, 38, 23, 4);
    delay(10);

    // Try at 25 MHz first; drop to 4 MHz on failure (some cards need slower)
    if (!SD.begin(4, SPI, 25000000)) {
        Serial.println("[sd] 25 MHz init failed, retrying at 4 MHz...");
        SD.end();
        delay(50);
        if (!SD.begin(4, SPI, 4000000)) {
            Serial.println("[sd] Card init failed — not present or needs FAT32 format");
            sdReady    = false;
            sdCardInfo = "No card";
            return;
        }
    }

    sdReady = true;
    SD.mkdir("/wav");
    SD.mkdir("/vcons");
    uint64_t totalMB = SD.totalBytes() / (1024ULL * 1024ULL);
    uint64_t usedMB  = SD.usedBytes()  / (1024ULL * 1024ULL);
    sdCardInfo = String((uint32_t)totalMB) + "MB";
    Serial.printf("[sd] Ready — %llu MB total, %llu MB used\n", totalMB, usedMB);
    Serial.printf("[sd] Card type: %s\n",
        SD.cardType() == CARD_MMC  ? "MMC"  :
        SD.cardType() == CARD_SD   ? "SDSC" :
        SD.cardType() == CARD_SDHC ? "SDHC" : "UNKNOWN");
}

// Build a date/hour directory path from the current time.
// result: "/wav/2026/03/31/14"  or "/vcons/2026/03/31/14"
// Falls back to /YYYY/00/00/00 bucket when NTP is not yet available.
void buildDateDir(char* buf, size_t size, const char* root) {
    struct tm t;
    if (getLocalTime(&t)) {
        snprintf(buf, size, "%s/%04d/%02d/%02d/%02d",
                 root,
                 t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour);
    } else {
        snprintf(buf, size, "%s/0000/00/00/00", root);
    }
}

// Create every component of an absolute path (mkdir -p).
void mkdirP(const char* path) {
    char tmp[80];
    strncpy(tmp, path, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';
    for (char* p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            SD.mkdir(tmp);
            *p = '/';
        }
    }
    SD.mkdir(tmp);
}

// Find the lexicographically smallest child directory inside parentPath.
// Writes the child name (not full path) into childName.  Returns true if found.
// Skips regular files — only considers sub-directories.
bool findOldestChildDir(const char* parentPath, char* childName, size_t nameSize) {
    File dir = SD.open(parentPath);
    if (!dir || !dir.isDirectory()) return false;
    bool found = false;
    childName[0] = '\0';
    File entry;
    while ((entry = dir.openNextFile())) {
        if (entry.isDirectory()) {
            // On ESP32 Arduino 3.x, File::name() returns just the filename component.
            const char* n = entry.name();
            if (!found || strcmp(n, childName) < 0) {
                strncpy(childName, n, nameSize - 1);
                childName[nameSize - 1] = '\0';
                found = true;
            }
        }
        entry.close();
    }
    dir.close();
    return found;
}

// Delete all regular files in a directory (non-recursive).
// Re-opens the directory on each iteration to avoid iterator invalidation.
void deleteFilesInDir(const char* path) {
    for (int pass = 0; pass < 1000; pass++) {
        File dir = SD.open(path);
        if (!dir) return;
        bool deleted = false;
        File entry;
        while ((entry = dir.openNextFile())) {
            if (!entry.isDirectory()) {
                char fpath[128];
                snprintf(fpath, sizeof(fpath), "%s/%s", path, entry.name());
                entry.close();
                dir.close();
                SD.remove(fpath);
                Serial.printf("[sd] pruned: %s\n", fpath);
                deleted = true;
                break;   // restart iteration after deletion
            }
            entry.close();
        }
        if (!deleted) { dir.close(); break; }
        // dir was already closed in the branch above
    }
}

// Try to remove a directory (only succeeds when empty).
static inline void tryRmdir(const char* path) { SD.rmdir(path); }

// Delete the oldest hour-directory under root ("/wav" or "/vcons").
// Returns true if anything was actually removed.
bool pruneOldestHourDir(const char* root) {
    char yearName[8]  = "", monthName[4] = "";
    char dayName[4]   = "", hourName[4]  = "";
    char path[80];

    if (!findOldestChildDir(root, yearName, sizeof(yearName))) return false;

    snprintf(path, sizeof(path), "%s/%s", root, yearName);
    if (!findOldestChildDir(path, monthName, sizeof(monthName))) {
        tryRmdir(path);
        return false;
    }

    snprintf(path, sizeof(path), "%s/%s/%s", root, yearName, monthName);
    if (!findOldestChildDir(path, dayName, sizeof(dayName))) {
        char tmp[80]; snprintf(tmp, sizeof(tmp), "%s/%s", root, yearName);
        tryRmdir(path); tryRmdir(tmp);
        return false;
    }

    snprintf(path, sizeof(path), "%s/%s/%s/%s", root, yearName, monthName, dayName);
    if (!findOldestChildDir(path, hourName, sizeof(hourName))) {
        char p2[80], p1[80];
        snprintf(p2, sizeof(p2), "%s/%s/%s", root, yearName, monthName);
        snprintf(p1, sizeof(p1), "%s/%s", root, yearName);
        tryRmdir(path); tryRmdir(p2); tryRmdir(p1);
        return false;
    }

    char hourPath[80];
    snprintf(hourPath, sizeof(hourPath), "%s/%s/%s/%s/%s",
             root, yearName, monthName, dayName, hourName);
    Serial.printf("[sd] pruning: %s\n", hourPath);
    deleteFilesInDir(hourPath);
    tryRmdir(hourPath);

    // Clean up empty parents
    char p3[80], p2[80], p1[80];
    snprintf(p3, sizeof(p3), "%s/%s/%s/%s", root, yearName, monthName, dayName);
    snprintf(p2, sizeof(p2), "%s/%s/%s",    root, yearName, monthName);
    snprintf(p1, sizeof(p1), "%s/%s",       root, yearName);
    tryRmdir(p3); tryRmdir(p2); tryRmdir(p1);

    return true;
}

// Ensure at least SD_MIN_FREE_BYTES are available on the card.
// Deletes oldest hour-directories (alternating /wav and /vcons) until satisfied
// or nothing remains to delete.
void ensureSDSpace() {
    if (!sdReady) return;
    for (int safety = 0; safety < 96; safety++) {
        uint64_t freeBytes = SD.totalBytes() - SD.usedBytes();
        if (freeBytes >= SD_MIN_FREE_BYTES) break;
        Serial.printf("[sd] low space (%llu MB free) — pruning old files\n",
                      freeBytes / (1024ULL * 1024ULL));
        bool a = pruneOldestHourDir("/wav");
        bool b = pruneOldestHourDir("/vcons");
        if (!a && !b) break;   // nothing left to prune
    }
}

// Write raw WAV to /wav/YYYY/MM/DD/HH/<uuid>.wav
// Takes explicit buf/numSamples so it can be called from the upload task
// with the completed buffer without touching the recording globals.
bool saveWavToSD(const int16_t* buf, size_t numSamples, const char* uuid) {
    if (!sdReady || numSamples == 0) return false;

    ensureSDSpace();

    char dirPath[64];
    buildDateDir(dirPath, sizeof(dirPath), "/wav");
    mkdirP(dirPath);

    char path[120];
    snprintf(path, sizeof(path), "%s/%.36s.wav", dirPath, uuid);
    File f = SD.open(path, FILE_WRITE);
    if (!f) {
        Serial.printf("[sd] open(%s) failed\n", path);
        return false;
    }
    uint8_t hdr[44];
    writeWavHeader(hdr, (uint32_t)numSamples);
    f.write(hdr, 44);
    const size_t CHUNK = 4096;
    size_t remaining = numSamples * 2u;
    const uint8_t* src = (const uint8_t*)buf;
    while (remaining > 0) {
        size_t n = (remaining > CHUNK) ? CHUNK : remaining;
        f.write(src, n);
        src       += n;
        remaining -= n;
    }
    f.close();
    sdSaved++;
    Serial.printf("[sd] WAV saved: %s (%u bytes)\n",
                  path, 44u + (uint32_t)numSamples * 2u);
    return true;
}

// Write the complete vCon JSON buffer to /vcons/YYYY/MM/DD/HH/<uuid>.json.
// Called while jsonBuf is still valid, before http POST.
bool saveVConToSD(const char* uuid, const char* jsonBuf, size_t jsonLen) {
    if (!sdReady) return false;

    ensureSDSpace();

    char dirPath[64];
    buildDateDir(dirPath, sizeof(dirPath), "/vcons");
    mkdirP(dirPath);

    char path[120];
    snprintf(path, sizeof(path), "%s/%.36s.json", dirPath, uuid);
    File f = SD.open(path, FILE_WRITE);
    if (!f) {
        Serial.printf("[sd] open(%s) failed\n", path);
        return false;
    }
    f.write((const uint8_t*)jsonBuf, jsonLen);
    f.close();
    Serial.printf("[sd] vCon saved: %s (%zu bytes)\n", path, jsonLen);
    return true;
}

// =============================================================================
// NTP / Timestamps
// =============================================================================

void initNTP() {
    if (!wifiConnected) return;
    configTime(0, 0, "pool.ntp.org", "time.nist.gov");
    struct tm t;
    ntpSynced = false;
    for (int i = 0; i < 20; i++) {
        if (getLocalTime(&t)) { ntpSynced = true; break; }
        delay(500);
    }
    Serial.printf("[ntp] Synced: %s\n", ntpSynced ? "yes" : "no");
}

// ISO 8601 UTC timestamp: "2025-03-31T23:37:56Z"
void getTimestamp(char* buf, size_t size) {
    struct tm t;
    if (getLocalTime(&t)) {
        strftime(buf, size, "%Y-%m-%dT%H:%M:%SZ", &t);
    } else {
        unsigned long s = millis() / 1000;
        snprintf(buf, size, "1970-01-01T%02lu:%02lu:%02luZ",
                 (s / 3600) % 24, (s / 60) % 60, s % 60);
    }
}

// HH:MM:SS for display
void getTimeDisplay(char* buf, size_t size) {
    struct tm t;
    if (getLocalTime(&t)) {
        strftime(buf, size, "%H:%M:%S", &t);
    } else {
        unsigned long s = millis() / 1000;
        snprintf(buf, size, "%02lu:%02lu:%02lu",
                 (s / 3600) % 24, (s / 60) % 60, s % 60);
    }
}

// YYYY-MM-DD for display
void getDateDisplay(char* buf, size_t size) {
    struct tm t;
    if (getLocalTime(&t)) {
        strftime(buf, size, "%Y-%m-%d", &t);
    } else {
        snprintf(buf, size, "----/--/--");
    }
}

// =============================================================================
// UUID Generation (UUID v8 — custom timestamp + random)
// =============================================================================

void generateUUID(char* out) {
    // Use boot-time microseconds + two random 32-bit values
    uint32_t t_hi = (uint32_t)(esp_timer_get_time() >> 20);
    uint32_t t_lo = (uint32_t)(esp_timer_get_time() & 0xFFFFF);
    uint32_t r1   = esp_random();
    uint32_t r2   = esp_random();
    // UUID v8: xxxxxxxx-xxxx-8xxx-[89ab]xxx-xxxxxxxxxxxx
    snprintf(out, 37,
             "%08x-%04x-8%03x-%04x-%04x%08x",
             t_hi,
             (uint16_t)(t_lo & 0xFFFF),
             (r1 & 0x0FFF),
             ((r1 >> 16) & 0x3FFF) | 0x8000,
             (uint16_t)(r2 & 0xFFFF),
             r2);
}

// =============================================================================
// Audio Recording
// =============================================================================

// Free both audio buffers so they can be reallocated at a new size.
// Call only when not recording.
void freeAudioBuffers() {
    if (audioBufA) { free(audioBufA); audioBufA = nullptr; }
    if (audioBufB) { free(audioBufB); audioBufB = nullptr; }
    audioBuffer = nullptr;
    recordBuf   = nullptr;
}

bool allocateAudioBuffer() {
    // Free any existing buffer — size may have changed via 'dur' command
    if (audioBufA) { free(audioBufA); audioBufA = nullptr; audioBuffer = nullptr; }
    if (!psramFound()) {
        Serial.println("[audio] FATAL: No PSRAM. Core2 requires PSRAM for audio buffer.");
        lastStatus = "No PSRAM detected!";
        return false;
    }
    uint32_t sz = audioPcmBytes();
    audioBuffer = (int16_t*)ps_malloc(sz);
    if (!audioBuffer) {
        Serial.printf("[audio] ps_malloc(%u) failed. Free PSRAM: %u\n",
                      sz, ESP.getFreePsram());
        lastStatus = "PSRAM alloc failed!";
        return false;
    }
    audioBufA = audioBuffer;   // audioBufA is always the first (primary) buffer
    recordBuf = audioBufA;
    Serial.printf("[audio] PCM buffer A: %u bytes (%u s). Free PSRAM: %u\n",
                  sz, recordDurationSec, ESP.getFreePsram());
    return true;
}

// Allocate the second PSRAM buffer needed for continuous double-buffering.
bool allocateContinuousBuffers() {
    // If already allocated at the right size, reuse it
    if (audioBufB) return true;
    uint32_t sz = audioPcmBytes();
    audioBufB = (int16_t*)ps_malloc(sz);
    if (!audioBufB) {
        Serial.printf("[audio] ps_malloc buf B (%u) failed. Free PSRAM: %u\n",
                      sz, ESP.getFreePsram());
        return false;
    }
    Serial.printf("[audio] PCM buffer B: %u bytes (%u s). Free PSRAM: %u\n",
                  sz, recordDurationSec, ESP.getFreePsram());
    return true;
}

void startRecording() {
    if (!audioBuffer && !allocateAudioBuffer()) return;

    // In continuous mode use recordBuf; in normal mode use audioBuffer (= audioBufA)
    int16_t* buf = continuousMode ? recordBuf : audioBufA;
    if (!buf) buf = audioBuffer;   // safety fallback

    memset(buf, 0, audioPcmBytes());
    audioBuffer      = buf;   // recordAudioChunk() reads this global
    audioBufferIndex = 0;

    // Configure M5Unified microphone — always end() first for clean codec reset
    M5.Mic.end();
    auto mic_cfg = M5.Mic.config();
    mic_cfg.sample_rate   = SAMPLE_RATE;
    mic_cfg.dma_buf_count = 16;
    mic_cfg.dma_buf_len   = 512;
    M5.Mic.config(mic_cfg);
    M5.Mic.begin();
    delay(300);  // ES7210 codec needs ~250 ms to stabilise after reset

    isRecording   = true;
    recordStartMs = millis();
    appState      = continuousMode ? STATE_CONTINUOUS : STATE_RECORDING;
    lastStatus    = continuousMode
                    ? "Continuous: chunk 1..."
                    : "Recording " + String(recordDurationSec) + " s...";

    memset(levelHistory, 0, sizeof(levelHistory));
    levelIdx = 0;

    Serial.printf("[audio] Recording started (continuous=%d). Target: %u s\n",
                  (int)continuousMode, recordDurationSec);
}

// Call from loop() while STATE_RECORDING or STATE_CONTINUOUS.
// Fills audio in 1024-sample chunks from the DMA ring buffer.
void recordAudioChunk() {
    if (!isRecording) return;

    size_t remaining = audioSampleTarget() - audioBufferIndex;
    if (remaining == 0) {
        if (continuousMode) {
            // Don't stop — swapAndContinue() in loop() will handle the swap.
            // If the upload task is still busy we'll just keep accumulating
            // a tiny bit past the boundary (handled by overflow guard below).
            return;
        }
        M5.Mic.end();
        isRecording = false;
        appState    = STATE_ENCODING;
        Serial.printf("[audio] Buffer full: %zu samples in %lu ms\n",
                      audioBufferIndex, millis() - recordStartMs);
        return;
    }

    size_t chunk = min((size_t)1024, remaining);
    // M5Unified 0.2.13: Mic.record() is NON-BLOCKING — it queues the buffer
    // for async DMA fill and returns immediately.  Reading sramChunk right after
    // the call would return stale zeros.  Fix: wait until isRecording()==0
    // (both internal DMA slots drained) before reading the buffer.
    //
    // M5Unified already applies f_gain = magnification/(over_sampling×2) = 4×
    // internally.  Do NOT add additional software gain here — double-gain causes
    // hard clipping that sounds like heavy distortion on normal speech.
    static int16_t DRAM_ATTR sramChunk[1024];   // 2 KB in internal SRAM
    if (M5.Mic.record(sramChunk, chunk, SAMPLE_RATE)) {
        // Wait for the DMA task to finish filling sramChunk (≤ ~200 ms at 8 kHz).
        uint32_t t0 = millis();
        while (M5.Mic.isRecording() && (millis() - t0 < 500)) { delay(1); }

        // Track peak for VU meter — no gain modification (M5Unified handles it)
        int peakVal = 0;
        for (size_t i = 0; i < chunk; i++) {
            int av = (int)sramChunk[i];
            if (av < 0) av = -av;
            if (av > peakVal) peakVal = av;
        }
        // Copy filled SRAM chunk → PSRAM main buffer
        memcpy(&audioBuffer[audioBufferIndex], sramChunk, chunk * sizeof(int16_t));

        // Store peak directly as int (levelHistory is int[])
        levelHistory[levelIdx % LEVEL_BARS] = peakVal;
        levelIdx++;
        audioBufferIndex += chunk;
    } else {
        // Mic not ready yet — normal during the first few ms after begin()
        delay(1);
    }

    // Check elapsed time
    if (millis() - recordStartMs >= (unsigned long)recordDurationSec * 1000UL) {
        if (!continuousMode) {
            M5.Mic.end();
            isRecording = false;
            appState    = STATE_ENCODING;
            Serial.printf("[audio] Duration reached: %zu samples in %lu ms\n",
                          audioBufferIndex, millis() - recordStartMs);
        }
        // In continuous mode the swap is handled by swapAndContinue() in loop()
    }
}

float recordingProgress() {
    unsigned long elapsed = millis() - recordStartMs;
    return min(1.0f, (float)elapsed / ((float)recordDurationSec * 1000.0f));
}

// =============================================================================
// WAV Header
// =============================================================================

void writeWavHeader(uint8_t* buf, uint32_t numSamples) {
    uint32_t dataBytes  = numSamples * 2;           // 16-bit mono
    uint32_t chunkSize  = 36 + dataBytes;
    uint32_t sr         = (uint32_t)SAMPLE_RATE;
    uint32_t byteRate   = sr * 2;
    uint16_t blockAlign = 2;
    uint16_t numChans   = 1;
    uint16_t bps        = 16;
    uint16_t audioFmt   = 1;   // PCM
    uint32_t fmtChunk   = 16;

    memcpy(buf +  0, "RIFF",    4);
    memcpy(buf +  4, &chunkSize, 4);
    memcpy(buf +  8, "WAVE",    4);
    memcpy(buf + 12, "fmt ",    4);
    memcpy(buf + 16, &fmtChunk, 4);
    memcpy(buf + 20, &audioFmt, 2);
    memcpy(buf + 22, &numChans, 2);
    memcpy(buf + 24, &sr,       4);
    memcpy(buf + 28, &byteRate, 4);
    memcpy(buf + 32, &blockAlign, 2);
    memcpy(buf + 34, &bps,      2);
    memcpy(buf + 36, "data",    4);
    memcpy(buf + 40, &dataBytes, 4);
}

// =============================================================================
// Base64url Encoding
// =============================================================================

// Encodes `inputLen` bytes of `input` into pre-allocated `outBuf` (size outBufSize).
// Converts standard base64 (+, /) to base64url (-, _) and strips = padding.
// Returns actual output length (no null terminator counted).
size_t encodeBase64url(const uint8_t* input, size_t inputLen,
                       uint8_t* outBuf, size_t outBufSize) {
    size_t b64Len = 0;
    int ret = mbedtls_base64_encode(outBuf, outBufSize, &b64Len, input, inputLen);
    if (ret != 0) {
        Serial.printf("[b64] mbedtls_base64_encode error: %d\n", ret);
        return 0;
    }
    // In-place convert to base64url, strip trailing '='
    size_t finalLen = 0;
    for (size_t i = 0; i < b64Len; i++) {
        uint8_t c = outBuf[i];
        if (c == '=') break;
        if (c == '+') c = '-';
        else if (c == '/') c = '_';
        outBuf[finalLen++] = c;
    }
    outBuf[finalLen] = '\0';
    return finalLen;
}

// =============================================================================
// JSON String Escaping
// =============================================================================

// Escapes `src` for safe embedding as a JSON string value into `dst`.
// Replaces " → \" and \ → \\.  Returns bytes written (excluding null).
size_t jsonEscape(char* dst, size_t dstSize, const char* src) {
    size_t j = 0;
    for (size_t i = 0; src[i] && j < dstSize - 3; i++) {
        char c = src[i];
        if (c == '"' || c == '\\') {
            dst[j++] = '\\';
        }
        dst[j++] = c;
    }
    dst[j] = '\0';
    return j;
}

// =============================================================================
// Build vCon JSON and POST
// =============================================================================
//
// buildAndUploadVConCore(buf, numSamples)
//   The inner implementation.  Takes an explicit buffer + sample count so it
//   can be called from both the main loop (normal mode) and a background
//   FreeRTOS task (continuous mode).  Does NOT call updateDisplay() — all
//   state changes are written to volatile primitives read by the main loop.
//
// buildAndUploadVCon()
//   Thin wrapper for normal (single-shot) mode; calls the core with globals.
//
// uploadTaskFn() / startUploadTask()
//   FreeRTOS task that runs buildAndUploadVConCore on core 0 while recording
//   continues on core 1.  Used exclusively by continuous mode.
// =============================================================================

bool buildAndUploadVConCore(const int16_t* buf, size_t numSamples) {
    if (numSamples == 0) return false;
    if (!wifiConnected)  return false;

    Serial.printf("[vcon] Building vCon. Samples:%zu FreePS:%u\n",
                  numSamples, ESP.getFreePsram());

    // ---- Gather metadata (stack buffers) ----------------------------------
    char timestamp[32];
    getTimestamp(timestamp, sizeof(timestamp));

    char uuidStr[37];
    generateUUID(uuidStr);
    lastVconUUID = String(uuidStr);

    String macStr = WiFi.macAddress();

    char tagsRaw[320];
    snprintf(tagsRaw, sizeof(tagsRaw),
             "{\"source\":\"m5stack-core2\","
             "\"device_id\":\"%s\","
             "\"mac\":\"%s\","
             "\"sample_rate\":%d,"
             "\"duration_seconds\":%u}",
             deviceID.c_str(), macStr.c_str(), SAMPLE_RATE,
             (unsigned)(numSamples / SAMPLE_RATE));
    char tagsEsc[512];
    jsonEscape(tagsEsc, sizeof(tagsEsc), tagsRaw);

    char prefix[1024];
    int prefixLen = snprintf(prefix, sizeof(prefix),
        "{"
          "\"vcon\":\"0.4.0\","
          "\"uuid\":\"%s\","
          "\"created_at\":\"%s\","
          "\"extensions\":[\"meta\"],"
          "\"parties\":[{"
            "\"name\":\"M5Stack Recorder\","
            "\"role\":\"recorder\","
            "\"meta\":{"
              "\"device_id\":\"%s\","
              "\"vconic_id\":\"%s\","
              "\"device_type\":\"m5stack-core2\""
            "}"
          "}],"
          "\"dialog\":[{"
            "\"type\":\"recording\","
            "\"start\":\"%s\","
            "\"duration\":%u,"
            "\"parties\":[0],"
            "\"originator\":0,"
            "\"mimetype\":\"audio/wav\","
            "\"encoding\":\"base64url\","
            "\"body\":\"",
        // Note: device_id MUST be the full colon-format MAC for portal MAC routing
        uuidStr, timestamp, macStr.c_str(), deviceID.c_str(), timestamp,
        (unsigned)(numSamples / SAMPLE_RATE));

    char suffix[640];
    int suffixLen = snprintf(suffix, sizeof(suffix),
        "\"}],"
        "\"analysis\":[],"
        "\"attachments\":[{"
          "\"purpose\":\"tags\","
          "\"start\":\"%s\","
          "\"party\":0,"
          "\"dialog\":0,"
          "\"encoding\":\"json\","
          "\"body\":\"%s\""
        "}]}",
        timestamp, tagsEsc);

    // ---- Save WAV to SD (before large PSRAM allocs) ----------------------
    uploadTaskState = UTS_SD;
    if (sdReady) saveWavToSD(buf, numSamples, uuidStr);

    // ---- Allocate JSON buffer and encode WAV ----------------------------
    uploadTaskState = UTS_ENCODING;

    uint32_t wavBytes  = 44u + (uint32_t)numSamples * 2u;
    size_t b64StdLen   = ((size_t)(wavBytes + 2u) / 3u) * 4u;
    size_t jsonBufSize = (size_t)prefixLen + b64StdLen + (size_t)suffixLen + 8u;

    char* jsonBuf = (char*)ps_malloc(jsonBufSize);
    if (!jsonBuf) {
        Serial.printf("[vcon] ps_malloc json (%zu) failed. FreePS:%u\n",
                      jsonBufSize, ESP.getFreePsram());
        uploadTaskState = UTS_DONE_FAIL;
        return false;
    }
    memcpy(jsonBuf, prefix, prefixLen);

    uint8_t* wavBuf = (uint8_t*)ps_malloc(wavBytes);
    if (!wavBuf) {
        free(jsonBuf);
        Serial.printf("[vcon] ps_malloc WAV (%u) failed. FreePS:%u\n",
                      wavBytes, ESP.getFreePsram());
        uploadTaskState = UTS_DONE_FAIL;
        return false;
    }
    writeWavHeader(wavBuf, (uint32_t)numSamples);
    memcpy(wavBuf + 44, buf, numSamples * 2u);

    // Encode straight into jsonBuf[prefixLen]; mbedtls writes standard base64
    size_t b64OutLen   = 0;
    size_t b64Region   = jsonBufSize - (size_t)prefixLen - (size_t)suffixLen - 4u;
    int    mbedRet     = mbedtls_base64_encode(
                            (uint8_t*)jsonBuf + prefixLen, b64Region,
                            &b64OutLen, wavBuf, wavBytes);
    free(wavBuf);   // done with raw WAV

    if (mbedRet != 0) {
        free(jsonBuf);
        Serial.printf("[vcon] mbedtls error: %d\n", mbedRet);
        uploadTaskState = UTS_DONE_FAIL;
        return false;
    }

    // In-place convert standard base64 → base64url, strip '=' padding
    size_t b64UrlLen = 0;
    uint8_t* b64Start = (uint8_t*)jsonBuf + prefixLen;
    for (size_t i = 0; i < b64OutLen; i++) {
        uint8_t c = b64Start[i];
        if (c == '=') break;
        if (c == '+') c = '-';
        else if (c == '/') c = '_';
        b64Start[b64UrlLen++] = c;
    }

    // Append suffix immediately after base64url data
    memcpy(jsonBuf + prefixLen + b64UrlLen, suffix, suffixLen);
    size_t jsonLen = (size_t)prefixLen + b64UrlLen + (size_t)suffixLen;
    jsonBuf[jsonLen] = '\0';

    Serial.printf("[vcon] JSON: %zu bytes  b64url:%zu  FreePS:%u\n",
                  jsonLen, b64UrlLen, ESP.getFreePsram());

    // ---- Save vCon JSON to SD -------------------------------------------
    if (sdReady) saveVConToSD(uuidStr, jsonBuf, jsonLen);

    // ---- HTTP POST with retry -------------------------------------------
    uploadTaskState = UTS_UPLOADING;

    String urlFinal = postURL;
    if (deviceToken.length() > 0) {
        urlFinal += (urlFinal.indexOf('?') >= 0) ? "&" : "?";
        urlFinal += "token=" + deviceToken;
    }
    Serial.printf("[vcon] POST → %s\n", urlFinal.c_str());

    // Retry policy (per portal spec):
    //   202 or 200 → success, stop
    //   400        → permanent failure, stop
    //   network / 5xx → retry 3× with 5 s / 30 s / 300 s back-off
    const unsigned long retryDelayMs[3] = { 5000UL, 30000UL, 300000UL };
    int httpCode = 0;

    for (int attempt = 0; attempt <= 3; attempt++) {
        if (attempt > 0) {
            uploadTaskState = UTS_RETRY;
            uploadRetryNum  = (uint8_t)attempt;
            unsigned long waitMs = retryDelayMs[attempt - 1];
            Serial.printf("[vcon] Waiting %lu ms before retry %d\n", waitMs, attempt);
            unsigned long t0 = millis();
            while (millis() - t0 < waitMs) { yield(); delay(10); }
            uploadTaskState = UTS_UPLOADING;
        }

        HTTPClient http;
        http.begin(urlFinal);
        http.addHeader("Content-Type", "application/json");
        if (deviceToken.length() > 0)
            http.addHeader("X-Device-Token", deviceToken);
        http.setTimeout(60000);

        httpCode = http.POST((uint8_t*)jsonBuf, jsonLen);
        if (httpCode > 0) {
            String resp = http.getString();
            if (resp.length() > 80) resp = resp.substring(0, 77) + "...";
            Serial.printf("[vcon] HTTP %d: %s\n", httpCode, resp.c_str());
        } else {
            Serial.printf("[vcon] Network error: %d\n", httpCode);
        }
        http.end();

        if (httpCode == 202 || httpCode == 200) break;
        if (httpCode == 400) break;
    }

    uploadTaskHttpCode = (int32_t)httpCode;
    free(jsonBuf);

    if (httpCode == 202 || httpCode == 200) {
        totalUploads++;
        saveConfig();
        uploadTaskState = UTS_DONE_OK;
        Serial.printf("[vcon] SUCCESS HTTP %d  UUID:%s\n", httpCode, uuidStr);
        return true;
    } else {
        failedUploads++;
        saveConfig();
        uploadTaskState = UTS_DONE_FAIL;
        Serial.printf("[vcon] FAILED HTTP %d\n", httpCode);
        return false;
    }
}

// Normal (single-shot) mode wrapper.
bool buildAndUploadVCon() {
    if (audioBufferIndex == 0) { lastStatus = "No audio recorded"; return false; }
    if (!wifiConnected)         { lastStatus = "No WiFi connection"; return false; }

    appState   = STATE_ENCODING;
    lastStatus = "Encoding audio...";
    updateDisplay();

    uploadTaskState    = UTS_IDLE;
    uploadTaskHttpCode = 0;

    bool ok = buildAndUploadVConCore(audioBuffer, audioBufferIndex);

    lastHttpCode  = (int)uploadTaskHttpCode;
    if (ok) {
        lastStatus    = (lastHttpCode == 202) ? "vCon routed! HTTP 202"
                                              : "vCon accepted HTTP 200";
        appState      = STATE_SUCCESS;
        stateChangeMs = millis();
    } else {
        lastStatus    = "Upload failed: HTTP " + String(lastHttpCode);
        appState      = STATE_ERROR;
        stateChangeMs = millis();
    }
    return ok;
}

// =============================================================================
// Continuous mode — FreeRTOS upload task (core 0) + swap logic
// =============================================================================

void uploadTaskFn(void* /*param*/) {
    buildAndUploadVConCore(g_uploadJob.buf, g_uploadJob.numSamples);
    continuousChunks++;
    uploadBusy = false;
    vTaskDelete(nullptr);
}

void startUploadTask(int16_t* buf, size_t numSamples) {
    g_uploadJob.buf        = buf;
    g_uploadJob.numSamples = numSamples;
    uploadBusy             = true;
    uploadTaskState        = UTS_ENCODING;
    xTaskCreatePinnedToCore(
        uploadTaskFn, "vcon_up",
        16 * 1024, nullptr, 1,
        &g_uploadTaskHandle, 0);   // core 0
}

// Called every loop() tick in continuous mode.
void swapAndContinue() {
    bool durationDone = (millis() - recordStartMs >=
                         (unsigned long)recordDurationSec * 1000UL);
    bool bufferFull   = (audioBufferIndex >= audioSampleTarget());

    if (!(durationDone || bufferFull)) return;

    if (uploadBusy) {
        if (bufferFull) {
            // Hard limit — stop mic and spinwait for the upload to finish
            M5.Mic.end(); isRecording = false;
            Serial.println("[cont] upload busy at buffer limit — waiting...");
            while (uploadBusy) { yield(); delay(5); }
            M5.Mic.begin(); isRecording = true;
        }
        return;
    }

    // Swap buffers
    int16_t* doneBuf     = recordBuf;
    size_t   doneSamples = audioBufferIndex;

    recordBuf        = (recordBuf == audioBufA) ? audioBufB : audioBufA;
    audioBuffer      = recordBuf;
    memset(audioBuffer, 0, audioPcmBytes());
    audioBufferIndex = 0;
    recordStartMs    = millis();

    Serial.printf("[cont] swap → uploading %zu samples; recording into %s\n",
                  doneSamples, (recordBuf == audioBufA) ? "bufA" : "bufB");

    startUploadTask(doneBuf, doneSamples);

    if (stopContinuous) {
        M5.Mic.end();
        isRecording    = false;
        continuousMode = false;
        stopContinuous = false;
        appState       = STATE_UPLOADING;
        lastStatus     = "Finishing last chunk...";
    } else {
        lastStatus = "Continuous: chunk " +
                     String((uint32_t)continuousChunks + 1) + "...";
    }
}

// =============================================================================
// Display helpers
// =============================================================================

// Horizontal hazard tape (alternating 4 px VConic-green / 4 px black stripes)
void drawHazard(int x, int y, int w, int h) {
    for (int i = 0; i < w; i++) {
        uint16_t col = ((i / 4) % 2 == 0) ? VC_GREEN : 0x2000u;
        M5.Display.drawFastVLine(x + i, y, h, col);
    }
}

// Panel: clear interior, draw border + label bar
void drawPanel(int x, int y, int w, int h, const char* label,
               uint16_t labelBg   = VC_GREEN,
               uint16_t labelFg   = TFT_BLACK,
               uint16_t borderCol = VC_GREEN) {
    M5.Display.fillRect(x + 1, y + 1, w - 2, h - 2, TFT_BLACK);
    M5.Display.drawRect(x, y, w, h, borderCol);
    M5.Display.fillRect(x + 1, y + 1, w - 2, 11, labelBg);
    M5.Display.setTextSize(1);
    M5.Display.setTextColor(labelFg, labelBg);
    M5.Display.setCursor(x + 3, y + 3);
    M5.Display.print(label);
}

// 16-bar VU meter inside the AUDIO panel
void drawLevelBars(int x, int y, int w, int h) {
    int barW = (w - LEVEL_BARS) / LEVEL_BARS;   // e.g. (158-16)/16 = 8 px
    int gap  = 1;

    // One fillRect clears the entire bars region — gaps, right-side remainder,
    // and any stale pixels from previous screens.  Because updateDisplay() holds
    // startWrite() for its whole duration, this clear and every subsequent bar
    // fill arrive at the display controller as one SPI burst: no visible flash.
    M5.Display.fillRect(x, y, w, h, TFT_BLACK);

    if (appState != STATE_RECORDING) {
        // Show a dim flat-line so the panel doesn't look dead when idle
        M5.Display.drawFastHLine(x, y + h / 2, w, 0x2945u);
        return;
    }

    for (int i = 0; i < LEVEL_BARS; i++) {
        // Roll: oldest bar on the left, newest on the right
        int idx   = (levelIdx - LEVEL_BARS + i + LEVEL_BARS * 2) % LEVEL_BARS;
        int level = levelHistory[idx];
        int barH  = (int)((float)level / 32767.0f * (float)h);
        int bx    = x + i * (barW + gap);
        if (barH > 0) {
            uint16_t col = TFT_GREEN;
            if      (barH > h * 70 / 100) col = TFT_RED;
            else if (barH > h * 40 / 100) col = TFT_YELLOW;
            M5.Display.fillRect(bx, y + h - barH, barW, barH, col);
        }
    }
}

// Progress bar (filled rect + outline)
void drawProgressBar(int x, int y, int w, int h, float pct, uint16_t fillCol = TFT_CYAN) {
    M5.Display.drawRect(x, y, w, h, TFT_WHITE);
    int filled = (int)(pct * (float)(w - 2));
    if (filled > 0)
        M5.Display.fillRect(x + 1, y + 1, filled, h - 2, fillCol);
    M5.Display.fillRect(x + 1 + filled, y + 1, w - 2 - filled, h - 2, TFT_BLACK);
}

// =============================================================================
// UI: Button row helper
// =============================================================================

void drawButtonRow(const char* labelA, const char* labelB, const char* labelC,
                   uint16_t colA = VC_GREEN,
                   uint16_t colB = VC_GREEN,
                   uint16_t colC = VC_GREEN) {
    M5.Display.fillRect(0, UI_BTN_Y, SCREEN_W, UI_BTN_H, TFT_BLACK);
    M5.Display.drawFastHLine(0, UI_BTN_Y, SCREEN_W, VC_GREEN);
    const int btnW = SCREEN_W / 3;
    const char* labels[3] = { labelA, labelB, labelC };
    uint16_t    colors[3] = { colA,   colB,   colC   };
    for (int i = 0; i < 3; i++) {
        int bx = i * btnW;
        if (i < 2) M5.Display.drawFastVLine(bx + btnW, UI_BTN_Y + 1, UI_BTN_H - 1, VC_GREEN);
        M5.Display.drawRect(bx + 4, UI_BTN_Y + 4, btnW - 8, UI_BTN_H - 8, colors[i]);
        M5.Display.setTextSize(2);
        int lw = (int)strlen(labels[i]) * 12;
        M5.Display.setTextColor(colors[i], TFT_BLACK);
        M5.Display.setCursor(bx + (btnW - lw) / 2, UI_BTN_Y + 14);
        M5.Display.print(labels[i]);
    }
}

// =============================================================================
// UI: Home screen
// =============================================================================

void drawHomeScreen() {
    bool activeRec = (appState == STATE_RECORDING || appState == STATE_CONTINUOUS);
    bool idle      = (appState == STATE_IDLE || appState == STATE_SUCCESS || appState == STATE_ERROR);

    // ── STATE BAND (y=8..128) — full-width, colour-coded ──────────────────
    uint16_t stateBg  = 0x1082u;    // near-black for IDLE
    uint16_t stateFg  = VC_GREEN;
    const char* stateStr = "IDLE";
    if (appState == STATE_RECORDING)  { stateBg = TFT_RED;   stateFg = TFT_WHITE; stateStr = "RECORDING"; }
    if (appState == STATE_CONTINUOUS) { stateBg = TFT_RED;   stateFg = TFT_WHITE; stateStr = "LIVE";      }
    if (appState == STATE_ENCODING)   { stateBg = 0x0451u;   stateFg = TFT_CYAN;  stateStr = "ENCODING";  }
    if (appState == STATE_UPLOADING)  { stateBg = 0x0451u;   stateFg = TFT_CYAN;  stateStr = "UPLOADING"; }
    if (appState == STATE_SUCCESS)    { stateBg = 0x0320u;   stateFg = TFT_GREEN; stateStr = "SUCCESS";   }
    if (appState == STATE_ERROR)      { stateBg = TFT_RED;   stateFg = TFT_WHITE; stateStr = "ERROR";     }

    M5.Display.fillRect(0, UI_CONTENT_Y, SCREEN_W, 120, stateBg);

    // Device ID — small, top-left
    M5.Display.setTextSize(1);
    M5.Display.setTextColor(stateFg == TFT_WHITE ? 0xBDF7u : 0x4208u, stateBg);
    M5.Display.setCursor(6, UI_CONTENT_Y + 4);
    M5.Display.print(deviceID.c_str());

    // WiFi dot — top-right
    M5.Display.setTextColor(wifiConnected ? TFT_GREEN : TFT_RED, stateBg);
    M5.Display.setCursor(SCREEN_W - 54, UI_CONTENT_Y + 4);
    M5.Display.print(wifiConnected ? "\x07 WiFi" : "\x18 WiFi");

    // Big state label — vertically centred in band
    M5.Display.setTextSize(4);
    M5.Display.setTextColor(stateFg, stateBg);
    int sw = (int)strlen(stateStr) * 24;
    M5.Display.setCursor((SCREEN_W - sw) / 2, UI_CONTENT_Y + 36);
    M5.Display.print(stateStr);

    // Progress bar + timer when recording
    if (activeRec) {
        float prog = recordingProgress();
        uint16_t barCol = (appState == STATE_CONTINUOUS) ? VC_GREEN : TFT_RED;
        drawProgressBar(10, UI_CONTENT_Y + 88, SCREEN_W - 20, 8, prog, barCol);
        char timerbuf[20];
        unsigned long elapsed = (millis() - recordStartMs) / 1000;
        snprintf(timerbuf, sizeof(timerbuf), "%02lu:%02lu / %02u:%02u",
                 elapsed / 60, elapsed % 60,
                 recordDurationSec / 60, recordDurationSec % 60);
        M5.Display.setTextSize(1);
        M5.Display.setTextColor(stateFg, stateBg);
        int tw = (int)strlen(timerbuf) * 6;
        M5.Display.setCursor((SCREEN_W - tw) / 2, UI_CONTENT_Y + 103);
        M5.Display.print(timerbuf);
        if (appState == STATE_CONTINUOUS) {
            M5.Display.setTextColor(VC_GREEN, stateBg);
            M5.Display.setCursor(SCREEN_W - 70, UI_CONTENT_Y + 103);
            M5.Display.printf("Chunk#%u", (uint32_t)continuousChunks + 1);
        }
    }

    // ── STATS BAND (y=128..163) ────────────────────────────────────────────
    M5.Display.fillRect(0, 128, SCREEN_W, 35, TFT_BLACK);
    M5.Display.drawFastHLine(0, 128, SCREEN_W, VC_GREEN);
    M5.Display.drawFastHLine(0, 163, SCREEN_W, 0x2945u);

    M5.Display.setTextSize(2);
    M5.Display.setTextColor(TFT_GREEN, TFT_BLACK);
    M5.Display.setCursor(8, 135);
    M5.Display.printf("OK:%-5u", totalUploads);
    M5.Display.setTextColor(TFT_RED, TFT_BLACK);
    M5.Display.setCursor(152, 135);
    M5.Display.printf("Err:%u", failedUploads);

    // Upload task state badge (right side, when active)
    if (appState == STATE_CONTINUOUS || appState == STATE_UPLOADING) {
        const char* utsStr = "";
        uint16_t utsCol = TFT_DARKGREY;
        switch (uploadTaskState) {
            case UTS_ENCODING:  utsStr = "ENC";  utsCol = TFT_CYAN;   break;
            case UTS_SD:        utsStr = "SD";   utsCol = VC_GREEN;   break;
            case UTS_UPLOADING: utsStr = "POST"; utsCol = TFT_CYAN;   break;
            case UTS_RETRY:     utsStr = "RTRY"; utsCol = TFT_YELLOW; break;
            case UTS_DONE_OK:   utsStr = "OK";   utsCol = TFT_GREEN;  break;
            case UTS_DONE_FAIL: utsStr = "FAIL"; utsCol = TFT_RED;    break;
            default: break;
        }
        if (utsStr[0]) {
            M5.Display.setTextSize(1);
            M5.Display.setTextColor(utsCol, TFT_BLACK);
            M5.Display.setCursor(SCREEN_W - 40, 135);
            M5.Display.print(utsStr);
        }
    }

    // ── STATUS ROW (y=163..193) ────────────────────────────────────────────
    M5.Display.fillRect(0, 163, SCREEN_W, 31, TFT_BLACK);
    M5.Display.setTextSize(1);
    uint16_t statusCol = TFT_WHITE;
    if (appState == STATE_SUCCESS) statusCol = TFT_GREEN;
    if (appState == STATE_ERROR)   statusCol = TFT_RED;
    M5.Display.setTextColor(statusCol, TFT_BLACK);
    M5.Display.setCursor(6, 168);
    // Truncate to fit one line
    String st = lastStatus;
    if (st.length() > 52) st = st.substring(0, 49) + "...";
    M5.Display.print(st.c_str());

    // Last HTTP code + dur hint
    M5.Display.setTextColor(TFT_DARKGREY, TFT_BLACK);
    M5.Display.setCursor(6, 181);
    M5.Display.printf("dur:%us", recordDurationSec);
    if (lastHttpCode > 0) {
        M5.Display.setTextColor(lastHttpCode == 202 ? TFT_GREEN :
                                lastHttpCode == 200 ? TFT_YELLOW : TFT_RED,
                                TFT_BLACK);
        M5.Display.printf("  HTTP %d", lastHttpCode);
    }

    // ── BUTTON ROW ─────────────────────────────────────────────────────────
    if (idle) {
        drawButtonRow("RUN",    "CONFIG", "TOOLS",
                      TFT_GREEN, VC_GREEN, VC_GREEN);
    } else if (activeRec) {
        drawButtonRow("--",     "STOP",   "STATUS",
                      TFT_DARKGREY, TFT_RED, VC_GREEN);
    } else {
        drawButtonRow("--",     "--",     "STATUS",
                      TFT_DARKGREY, TFT_DARKGREY, VC_GREEN);
    }
}

// =============================================================================
// UI: Status screen
// =============================================================================

void drawStatusScreen() {
    bool activeRec = (appState == STATE_RECORDING || appState == STATE_CONTINUOUS);

    // ── Header bar (y=8..22) ───────────────────────────────────────────────
    M5.Display.fillRect(0, UI_CONTENT_Y, SCREEN_W, 14, VC_GREEN);
    M5.Display.setTextSize(1);
    M5.Display.setTextColor(TFT_BLACK, VC_GREEN);
    M5.Display.setCursor(4, UI_CONTENT_Y + 3);
    M5.Display.print("STATUS \x7e RECORDING DASHBOARD");

    // ── VU meter (y=22..62, 40px tall) ────────────────────────────────────
    const int vx = 0, vy = 22, vw = SCREEN_W, vh = 40;
    M5.Display.fillRect(vx, vy, vw, vh, TFT_BLACK);
    M5.Display.drawRect(vx, vy, vw, vh, activeRec ? TFT_RED : VC_GREEN);
    const int barCount = LEVEL_BARS;
    const int barW     = (vw - 2 - barCount) / barCount;
    if (activeRec) {
        for (int i = 0; i < barCount; i++) {
            int idx   = (levelIdx - barCount + i + barCount * 2) % barCount;
            int level = levelHistory[idx];
            int barH  = (int)((float)level / 32767.0f * (float)(vh - 2));
            if (barH < 1) barH = 1;
            int bx    = vx + 1 + i * (barW + 1);
            uint16_t col = TFT_GREEN;
            if      (barH > (vh-2) * 70 / 100) col = TFT_RED;
            else if (barH > (vh-2) * 40 / 100) col = TFT_YELLOW;
            M5.Display.fillRect(bx, vy + vh - 1 - barH, barW, barH, col);
        }
    } else {
        M5.Display.drawFastHLine(vx+1, vy + vh/2, vw-2, 0x2945u);
        M5.Display.setTextSize(1);
        M5.Display.setTextColor(TFT_DARKGREY, TFT_BLACK);
        M5.Display.setCursor(vx + vw/2 - 30, vy + vh/2 - 4);
        M5.Display.print("not recording");
    }

    // ── Elapsed / progress (y=63..76) ─────────────────────────────────────
    M5.Display.fillRect(0, 63, SCREEN_W, 14, TFT_BLACK);
    M5.Display.drawFastHLine(0, 63, SCREEN_W, 0x2945u);
    if (activeRec || appState == STATE_ENCODING || appState == STATE_UPLOADING) {
        unsigned long elapsed = activeRec ? (millis() - recordStartMs) / 1000 : 0;
        float prog = activeRec ? recordingProgress() : 1.0f;
        uint16_t barCol = (appState == STATE_CONTINUOUS) ? VC_GREEN :
                          (appState == STATE_RECORDING)  ? TFT_RED  : TFT_CYAN;
        drawProgressBar(4, 64, SCREEN_W - 60, 8, prog, barCol);
        M5.Display.setTextSize(1);
        M5.Display.setTextColor(TFT_YELLOW, TFT_BLACK);
        M5.Display.setCursor(SCREEN_W - 54, 65);
        M5.Display.printf("%02lu:%02lu", elapsed / 60, elapsed % 60);
    } else {
        M5.Display.setTextSize(1);
        M5.Display.setTextColor(TFT_DARKGREY, TFT_BLACK);
        M5.Display.setCursor(4, 65);
        M5.Display.print("Idle \x7e press HOME \x7e RUN");
    }

    // ── Upload state (y=77..90) ────────────────────────────────────────────
    M5.Display.fillRect(0, 77, SCREEN_W, 14, TFT_BLACK);
    M5.Display.drawFastHLine(0, 77, SCREEN_W, 0x2945u);
    M5.Display.setTextSize(1);
    const char* utsStr = "IDLE";
    uint16_t    utsCol = TFT_DARKGREY;
    switch (uploadTaskState) {
        case UTS_ENCODING:  utsStr = "ENCODING"; utsCol = TFT_CYAN;   break;
        case UTS_SD:        utsStr = "SD WRITE"; utsCol = VC_GREEN;   break;
        case UTS_UPLOADING: utsStr = "POSTING";  utsCol = TFT_CYAN;   break;
        case UTS_RETRY:     utsStr = "RETRY";    utsCol = TFT_YELLOW; break;
        case UTS_DONE_OK:   utsStr = "OK";       utsCol = TFT_GREEN;  break;
        case UTS_DONE_FAIL: utsStr = "FAILED";   utsCol = TFT_RED;    break;
        default: break;
    }
    M5.Display.setTextColor(utsCol, TFT_BLACK);
    M5.Display.setCursor(4, 80);
    M5.Display.printf("Upload: %-8s", utsStr);
    uint16_t httpCol = (lastHttpCode >= 200 && lastHttpCode < 300) ? TFT_GREEN :
                       (lastHttpCode == 0) ? TFT_DARKGREY : TFT_RED;
    M5.Display.setTextColor(httpCol, TFT_BLACK);
    M5.Display.setCursor(160, 80);
    if (lastHttpCode > 0) M5.Display.printf("HTTP %d", lastHttpCode);
    else                   M5.Display.print("HTTP --");

    // ── Counters: OK / Err / Chunk (y=91..104) ────────────────────────────
    M5.Display.fillRect(0, 91, SCREEN_W, 14, TFT_BLACK);
    M5.Display.drawFastHLine(0, 91, SCREEN_W, 0x2945u);
    M5.Display.setTextSize(1);
    M5.Display.setTextColor(TFT_GREEN,  TFT_BLACK);
    M5.Display.setCursor(4,   94); M5.Display.printf("OK:%u", totalUploads);
    M5.Display.setTextColor(TFT_RED,    TFT_BLACK);
    M5.Display.setCursor(80,  94); M5.Display.printf("Err:%u", failedUploads);
    M5.Display.setTextColor(VC_GREEN,   TFT_BLACK);
    M5.Display.setCursor(165, 94); M5.Display.printf("Chunk #%u", (uint32_t)continuousChunks);

    // ── WiFi (y=105..118) ─────────────────────────────────────────────────
    M5.Display.fillRect(0, 105, SCREEN_W, 14, TFT_BLACK);
    M5.Display.drawFastHLine(0, 105, SCREEN_W, 0x2945u);
    M5.Display.setTextSize(1);
    if (wifiConnected) {
        M5.Display.setTextColor(TFT_GREEN,  TFT_BLACK);
        M5.Display.setCursor(4, 108); M5.Display.print("WiFi \x07 ");
        M5.Display.setTextColor(TFT_WHITE,  TFT_BLACK);
        String s = wifiSSID; if (s.length() > 14) s = s.substring(0, 13) + "~";
        M5.Display.print(s.c_str());
        M5.Display.setTextColor(TFT_CYAN, TFT_BLACK);
        M5.Display.setCursor(160, 108);
        M5.Display.print(WiFi.localIP().toString().c_str());
        M5.Display.setTextColor(TFT_DARKGREY, TFT_BLACK);
        M5.Display.setCursor(270, 108);
        M5.Display.printf("%d", wifiRSSI);
    } else {
        M5.Display.setTextColor(TFT_RED, TFT_BLACK);
        M5.Display.setCursor(4, 108); M5.Display.print("WiFi \x18 NOT CONNECTED");
    }

    // ── Time / Date (y=119..132) ───────────────────────────────────────────
    M5.Display.fillRect(0, 119, SCREEN_W, 14, TFT_BLACK);
    M5.Display.drawFastHLine(0, 119, SCREEN_W, 0x2945u);
    M5.Display.setTextSize(1);
    M5.Display.setTextColor(VC_GREEN, TFT_BLACK);
    char tbuf[12], dbuf[12];
    getTimeDisplay(tbuf, sizeof(tbuf));
    getDateDisplay(dbuf, sizeof(dbuf));
    M5.Display.setCursor(4, 122);
    M5.Display.printf("%s UTC", tbuf);
    M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
    M5.Display.setCursor(160, 122);
    M5.Display.print(dbuf);

    // ── Uptime / PSRAM (y=133..146) ───────────────────────────────────────
    M5.Display.fillRect(0, 133, SCREEN_W, 14, TFT_BLACK);
    M5.Display.drawFastHLine(0, 133, SCREEN_W, 0x2945u);
    M5.Display.setTextSize(1);
    M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
    unsigned long sec = millis() / 1000;
    M5.Display.setCursor(4, 136);
    M5.Display.printf("Up %02lu:%02lu:%02lu", (sec/3600)%24, (sec/60)%60, sec%60);
    M5.Display.setTextColor(TFT_DARKGREY, TFT_BLACK);
    M5.Display.setCursor(160, 136);
    M5.Display.printf("PSRAM %uKB", ESP.getFreePsram() / 1024);

    // ── Mode / dur / SD (y=147..160) ──────────────────────────────────────
    M5.Display.fillRect(0, 147, SCREEN_W, 14, TFT_BLACK);
    M5.Display.drawFastHLine(0, 147, SCREEN_W, 0x2945u);
    M5.Display.setTextSize(1);
    bool contOK = (recordDurationSec <= CONT_MAX_DURATION_SEC);
    M5.Display.setTextColor(contOK ? TFT_GREEN : TFT_YELLOW, TFT_BLACK);
    M5.Display.setCursor(4, 150);
    M5.Display.printf("%s  %us", contOK ? "CONT" : "SINGLE", recordDurationSec);
    M5.Display.setTextColor(sdReady ? TFT_GREEN : TFT_RED, TFT_BLACK);
    M5.Display.setCursor(160, 150);
    M5.Display.printf("SD saved:%u", sdSaved);

    // ── Button row ─────────────────────────────────────────────────────────
    drawButtonRow("HOME", "--", "--",
                  VC_GREEN, TFT_DARKGREY, TFT_DARKGREY);
}

// =============================================================================
// UI: Config screen
// =============================================================================

void drawConfigScreen() {
    // Header
    M5.Display.fillRect(0, UI_CONTENT_Y, SCREEN_W, 14, VC_GREEN);
    M5.Display.setTextSize(1);
    M5.Display.setTextColor(TFT_BLACK, VC_GREEN);
    M5.Display.setCursor(4, UI_CONTENT_Y + 3);
    M5.Display.print("CONFIGURATION");

    int y = UI_CONTENT_Y + 16;
    const int lh = 14;
    M5.Display.fillRect(0, y, SCREEN_W, UI_CONTENT_H - 16, TFT_BLACK);

    auto kv = [&](const char* key, const char* val, uint16_t vc = TFT_WHITE) {
        M5.Display.setTextSize(1);
        M5.Display.setTextColor(VC_GREEN, TFT_BLACK);
        M5.Display.setCursor(4, y);
        M5.Display.print(key);
        M5.Display.setTextColor(vc, TFT_BLACK);
        M5.Display.setCursor(120, y);
        M5.Display.print(val);
        y += lh;
    };

    kv("Device ID:",   deviceID.c_str(),          VC_GREEN);
    kv("MAC:",         WiFi.macAddress().c_str(),  TFT_CYAN);
    kv("Firmware:",    FIRMWARE_VERSION,            TFT_WHITE);

    char durbuf[24];
    snprintf(durbuf, sizeof(durbuf), "%us (%s)",
             recordDurationSec,
             recordDurationSec <= CONT_MAX_DURATION_SEC ? "cont" : "single");
    kv("Duration:",    durbuf,  recordDurationSec <= CONT_MAX_DURATION_SEC ? TFT_GREEN : TFT_YELLOW);

    kv("WiFi SSID:",   wifiSSID.c_str(),           TFT_WHITE);
    kv("WiFi Status:", wifiConnected ? "CONNECTED" : "DISCONNECTED",
                       wifiConnected ? TFT_GREEN : TFT_RED);
    if (wifiConnected) {
        kv("IP:",       WiFi.localIP().toString().c_str(), TFT_CYAN);
        kv("RSSI:",     (String(wifiRSSI) + " dBm").c_str(), TFT_WHITE);
    }
    kv("NTP:",         ntpSynced ? "Synced" : "Not synced",
                       ntpSynced ? TFT_GREEN : TFT_RED);

    String tok = deviceToken.length() > 0 ? deviceToken : "(none)";
    kv("Token:",       tok.c_str(),
                       deviceToken.length() > 0 ? VC_GREEN : TFT_DARKGREY);

    kv("SD:",          sdReady ? sdCardInfo.c_str() : "No card",
                       sdReady ? TFT_GREEN : TFT_RED);

    // POST URL — word wrap at ~44 chars
    if (y < UI_CONTENT_Y + UI_CONTENT_H - 14) {
        M5.Display.setTextColor(TFT_YELLOW, TFT_BLACK);
        M5.Display.setCursor(4, y); M5.Display.print("POST URL:"); y += lh;
        M5.Display.setTextColor(TFT_MAGENTA, TFT_BLACK);
        String u = postURL;
        while (u.length() > 0 && y < UI_CONTENT_Y + UI_CONTENT_H - 2) {
            int take = min((int)u.length(), 44);
            M5.Display.setCursor(4, y);
            M5.Display.print(u.substring(0, take));
            u = u.substring(take);
            y += lh;
        }
    }

    // Button row
    drawButtonRow("HOME", "SET DUR", "WIFI",
                  VC_GREEN, VC_GREEN, VC_GREEN);
}

// =============================================================================
// UI: Duration picker screen
// =============================================================================

void drawDurationScreen() {
    // Header bar
    M5.Display.fillRect(0, UI_CONTENT_Y, SCREEN_W, 14, VC_GREEN);
    M5.Display.setTextSize(1);
    M5.Display.setTextColor(TFT_BLACK, VC_GREEN);
    M5.Display.setCursor(4, UI_CONTENT_Y + 3);
    M5.Display.print("RECORDING DURATION");
    M5.Display.fillRect(0, UI_CONTENT_Y + 14, SCREEN_W, UI_CONTENT_H - 14, TFT_BLACK);

    bool cont = (durEditPending <= CONT_MAX_DURATION_SEC);

    // ── Large centred value ──────────────────────────────────────────────────
    // Size-5 char: 6×8 × 5 = 30×40 px per glyph
    char nbuf[8];
    snprintf(nbuf, sizeof(nbuf), "%u", durEditPending);
    int numGlyphs = strlen(nbuf);
    int numW = numGlyphs * 30 + 30;   // digits + " s" (2 glyphs × 30/2 ≈ rough)
    int numX = (SCREEN_W - numW) / 2;
    if (numX < 4) numX = 4;
    M5.Display.setTextSize(5);
    M5.Display.setTextColor(cont ? TFT_GREEN : TFT_YELLOW, TFT_BLACK);
    M5.Display.setCursor(numX, 28);
    M5.Display.print(nbuf);
    M5.Display.setTextSize(2);
    M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
    M5.Display.print(" s");

    // ── Range bar (y=90) ────────────────────────────────────────────────────
    const int bx = 10, by = 92, bw = SCREEN_W - 20, bh = 12;
    float frac = (float)(durEditPending - MIN_RECORD_DURATION_SEC) /
                 (float)(MAX_RECORD_DURATION_SEC - MIN_RECORD_DURATION_SEC);
    drawProgressBar(bx, by, bw, bh, frac, cont ? TFT_GREEN : TFT_YELLOW);

    // Tick line at CONT_MAX_DURATION_SEC
    int contMaxX = bx + (int)((float)(CONT_MAX_DURATION_SEC - MIN_RECORD_DURATION_SEC) /
                               (float)(MAX_RECORD_DURATION_SEC - MIN_RECORD_DURATION_SEC) * bw);
    M5.Display.drawFastVLine(contMaxX, by - 5, bh + 10, TFT_CYAN);

    // Min / CONT_MAX / Max labels
    M5.Display.setTextSize(1);
    M5.Display.setTextColor(TFT_DARKGREY, TFT_BLACK);
    M5.Display.setCursor(bx, by + bh + 4);
    M5.Display.printf("%us", MIN_RECORD_DURATION_SEC);
    M5.Display.setTextColor(TFT_CYAN, TFT_BLACK);
    char cmLabel[8]; snprintf(cmLabel, sizeof(cmLabel), "%us", CONT_MAX_DURATION_SEC);
    int cmLabelX = contMaxX - (int)(strlen(cmLabel) * 6) / 2;
    M5.Display.setCursor(cmLabelX, by - 14);
    M5.Display.print(cmLabel);
    M5.Display.setTextColor(TFT_DARKGREY, TFT_BLACK);
    char maxLabel[8]; snprintf(maxLabel, sizeof(maxLabel), "%us", MAX_RECORD_DURATION_SEC);
    M5.Display.setCursor(bx + bw - (int)strlen(maxLabel) * 6, by + bh + 4);
    M5.Display.print(maxLabel);

    // ── Mode badge ──────────────────────────────────────────────────────────
    M5.Display.setTextSize(1);
    if (cont) {
        M5.Display.setTextColor(TFT_GREEN, TFT_BLACK);
        M5.Display.setCursor(10, 134);
        M5.Display.print("\x7e CONTINUOUS  (zero-gap dual-buffer)");
    } else {
        M5.Display.setTextColor(TFT_YELLOW, TFT_BLACK);
        M5.Display.setCursor(10, 134);
        M5.Display.print("\x7e SINGLE-SHOT  (>45s, uploads between chunks)");
    }

    // ── Hint line ───────────────────────────────────────────────────────────
    M5.Display.setTextColor(TFT_DARKGREY, TFT_BLACK);
    M5.Display.setCursor(10, 152);
    M5.Display.printf("Steps: 5 s  |  SAVE stores to flash");

    // Unsaved-change indicator
    if (durEditPending != recordDurationSec) {
        M5.Display.setTextColor(TFT_YELLOW, TFT_BLACK);
        M5.Display.setCursor(10, 166);
        M5.Display.printf("Current: %us  \x7e  pending: %us",
                          recordDurationSec, durEditPending);
    }

    drawButtonRow("LESS", "SAVE", "MORE",
                  VC_GREEN, TFT_GREEN, VC_GREEN);
}

// =============================================================================
// UI: Tools screen
// =============================================================================

void drawToolsScreen() {
    const char* itemLabels[TOOL_COUNT] = {
        "Test WiFi",
        "Test OTA",
        "Test POST",
        "Show SD",
        "Restart"
    };

    // Header
    M5.Display.fillRect(0, UI_CONTENT_Y, SCREEN_W, 14, VC_GREEN);
    M5.Display.setTextSize(1);
    M5.Display.setTextColor(TFT_BLACK, VC_GREEN);
    M5.Display.setCursor(4, UI_CONTENT_Y + 3);
    M5.Display.print("TOOLS \x7e DIAGNOSTICS");

    M5.Display.fillRect(0, UI_CONTENT_Y + 14, SCREEN_W, UI_CONTENT_H - 14, TFT_BLACK);

    if (toolsPhase == TPHASE_SELECT) {
        // Menu items (y=22..186, 5 items × 32px each)
        for (int i = 0; i < TOOL_COUNT; i++) {
            int iy = UI_CONTENT_Y + 14 + i * 33;
            bool sel = (i == (int)toolsSelectedItem);
            uint16_t bg = sel ? 0x1082u : TFT_BLACK;   // dark highlight
            M5.Display.fillRect(0, iy, SCREEN_W, 32, bg);
            if (sel) {
                M5.Display.fillRect(0, iy, 4, 32, VC_GREEN);  // left accent bar
                M5.Display.drawRect(0, iy, SCREEN_W, 32, VC_GREEN);
            }
            M5.Display.setTextSize(2);
            M5.Display.setTextColor(sel ? VC_GREEN : TFT_WHITE, bg);
            M5.Display.setCursor(12, iy + 8);
            M5.Display.print(itemLabels[i]);
            // Danger label for restart
            if (i == TOOL_RESTART) {
                M5.Display.setTextSize(1);
                M5.Display.setTextColor(TFT_RED, bg);
                M5.Display.setCursor(200, iy + 12);
                M5.Display.print("REBOOT");
            }
        }
        drawButtonRow("PREV", "RUN", "HOME",
                      VC_GREEN, TFT_GREEN, VC_GREEN);

    } else if (toolsPhase == TPHASE_CONFIRM) {
        M5.Display.setTextSize(2);
        M5.Display.setTextColor(TFT_RED, TFT_BLACK);
        M5.Display.setCursor(60, 80);
        M5.Display.print("Restart device?");
        M5.Display.setTextSize(1);
        M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
        M5.Display.setCursor(40, 110);
        M5.Display.print("This will reboot the M5Stack.");
        M5.Display.setCursor(40, 124);
        M5.Display.print("Any active recording will stop.");
        drawButtonRow("--", "CONFIRM", "CANCEL",
                      TFT_DARKGREY, TFT_RED, VC_GREEN);

    } else if (toolsPhase == TPHASE_RUNNING) {
        const char* itemLabels2[TOOL_COUNT] = {
            "Testing WiFi...", "Testing OTA...", "Testing POST...",
            "Reading SD...", "Restarting..."
        };
        M5.Display.setTextSize(2);
        M5.Display.setTextColor(TFT_CYAN, TFT_BLACK);
        M5.Display.setCursor(20, 80);
        M5.Display.print(itemLabels2[toolsSelectedItem]);
        // Animated dots
        int dots = (millis() / 400) % 4;
        M5.Display.setTextColor(TFT_YELLOW, TFT_BLACK);
        M5.Display.setCursor(20, 110);
        for (int d = 0; d < dots; d++) M5.Display.print(". ");
        drawButtonRow("--", "--", "--",
                      TFT_DARKGREY, TFT_DARKGREY, TFT_DARKGREY);

    } else {  // TPHASE_RESULT
        M5.Display.setTextSize(2);
        uint16_t resCol = toolsResultOK ? TFT_GREEN : TFT_RED;
        M5.Display.setTextColor(resCol, TFT_BLACK);
        M5.Display.setCursor(20, 40);
        M5.Display.print(toolsResultOK ? "PASS" : "FAIL");
        M5.Display.setTextSize(1);
        M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
        M5.Display.setCursor(20, 70); M5.Display.print(toolsResultLine1.c_str());
        M5.Display.setCursor(20, 86); M5.Display.print(toolsResultLine2.c_str());
        // Time remaining auto-clear bar
        unsigned long elapsed = millis() - toolsResultMs;
        float remaining = 1.0f - min(1.0f, (float)elapsed / (float)TOOLS_RESULT_TIMEOUT_MS);
        drawProgressBar(20, 110, SCREEN_W - 40, 6, remaining, TFT_DARKGREY);
        M5.Display.setTextSize(1);
        M5.Display.setTextColor(TFT_DARKGREY, TFT_BLACK);
        M5.Display.setCursor(20, 122);
        M5.Display.print("Auto-returns to menu...");
        drawButtonRow("--", "BACK", "HOME",
                      TFT_DARKGREY, VC_GREEN, VC_GREEN);
    }
}

// =============================================================================
// UI: WiFi picker screen
// =============================================================================

// Run a WiFi scan and populate the wifiScan* arrays.
void wifiRunScan() {
    wifiScanCount = 0;
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    delay(100);
    WiFi.mode(WIFI_STA);
    delay(100);
    int n = WiFi.scanNetworks();
    if (n > WIFI_MAX_SCAN) n = WIFI_MAX_SCAN;
    for (int i = 0; i < n; i++) {
        strncpy(wifiScanSSID[i], WiFi.SSID(i).c_str(), 32);
        wifiScanSSID[i][32] = '\0';
        wifiScanRSSI[i] = WiFi.RSSI(i);
        wifiScanOpen[i] = (WiFi.encryptionType(i) == WIFI_AUTH_OPEN);
    }
    WiFi.scanDelete();
    wifiScanCount = n;
    wifiSelIdx    = 0;
    wifiScrollTop = 0;
    Serial.printf("[wifi-picker] Scan found %d networks\n", n);
}

// Draw a small RSSI bar icon (4 bars) at (x, y), 20×10 px
static void drawRSSIBars(int x, int y, int32_t rssi) {
    // Map RSSI to 0–4 bars:  > -50 = 4,  > -60 = 3,  > -70 = 2,  > -80 = 1, else 0
    int bars = 0;
    if      (rssi > -50) bars = 4;
    else if (rssi > -60) bars = 3;
    else if (rssi > -70) bars = 2;
    else if (rssi > -80) bars = 1;
    for (int b = 0; b < 4; b++) {
        int bh = 3 + b * 2;   // bar heights: 3, 5, 7, 9
        int bx = x + b * 5;
        uint16_t col = (b < bars) ? TFT_GREEN : 0x2104u;  // dark grey
        M5.Display.fillRect(bx, y + (10 - bh), 4, bh, col);
    }
}

void drawWifiScreen() {
    // Header
    M5.Display.fillRect(0, UI_CONTENT_Y, SCREEN_W, 14, VC_GREEN);
    M5.Display.setTextSize(1);
    M5.Display.setTextColor(TFT_BLACK, VC_GREEN);
    M5.Display.setCursor(4, UI_CONTENT_Y + 3);
    M5.Display.print("WIFI PICKER");
    M5.Display.fillRect(0, UI_CONTENT_Y + 14, SCREEN_W, UI_CONTENT_H - 14, TFT_BLACK);

    if (wifiPhase == WPHASE_SCANNING) {
        M5.Display.setTextSize(2);
        M5.Display.setTextColor(TFT_CYAN, TFT_BLACK);
        M5.Display.setCursor(60, 80);
        M5.Display.print("Scanning...");
        int dots = (millis() / 400) % 4;
        M5.Display.setTextColor(TFT_YELLOW, TFT_BLACK);
        M5.Display.setCursor(60, 110);
        for (int d = 0; d < dots; d++) M5.Display.print(". ");
        drawButtonRow("--", "--", "--",
                      TFT_DARKGREY, TFT_DARKGREY, TFT_DARKGREY);

    } else if (wifiPhase == WPHASE_SELECT) {
        if (wifiScanCount == 0) {
            M5.Display.setTextSize(2);
            M5.Display.setTextColor(TFT_RED, TFT_BLACK);
            M5.Display.setCursor(40, 70);
            M5.Display.print("No networks");
            M5.Display.setTextSize(1);
            M5.Display.setTextColor(TFT_DARKGREY, TFT_BLACK);
            M5.Display.setCursor(40, 100);
            M5.Display.print("Press SCAN to retry");
            drawButtonRow("SCAN", "--", "BACK",
                          VC_GREEN, TFT_DARKGREY, VC_GREEN);
        } else {
            // Show up to 5 networks at a time
            const int visibleRows = 5;
            const int rowH = 30;
            int y = UI_CONTENT_Y + 16;

            // Ensure selected item is visible
            if (wifiSelIdx < wifiScrollTop) wifiScrollTop = wifiSelIdx;
            if (wifiSelIdx >= wifiScrollTop + visibleRows)
                wifiScrollTop = wifiSelIdx - visibleRows + 1;

            int endIdx = wifiScrollTop + visibleRows;
            if (endIdx > wifiScanCount) endIdx = wifiScanCount;

            for (int i = wifiScrollTop; i < endIdx; i++) {
                int ry = y + (i - wifiScrollTop) * rowH;
                bool sel = (i == (int)wifiSelIdx);
                uint16_t bg = sel ? 0x1082u : TFT_BLACK;
                M5.Display.fillRect(0, ry, SCREEN_W, rowH - 1, bg);
                if (sel) {
                    M5.Display.fillRect(0, ry, 4, rowH - 1, VC_GREEN);
                    M5.Display.drawRect(0, ry, SCREEN_W, rowH - 1, VC_GREEN);
                }
                // SSID (truncate to 26 chars)
                M5.Display.setTextSize(1);
                M5.Display.setTextColor(sel ? VC_GREEN : TFT_WHITE, bg);
                M5.Display.setCursor(10, ry + 4);
                String ssid = wifiScanSSID[i];
                if (ssid.length() > 26) ssid = ssid.substring(0, 24) + "..";
                M5.Display.print(ssid);
                // Lock icon for secured networks
                if (!wifiScanOpen[i]) {
                    M5.Display.setTextColor(TFT_YELLOW, bg);
                    M5.Display.setCursor(10, ry + 16);
                    M5.Display.print("\xf0");  // lock-ish char
                }
                // RSSI bars
                drawRSSIBars(SCREEN_W - 30, ry + 6, wifiScanRSSI[i]);
                // dBm label
                M5.Display.setTextSize(1);
                M5.Display.setTextColor(TFT_DARKGREY, bg);
                char rssiLabel[8];
                snprintf(rssiLabel, sizeof(rssiLabel), "%d", (int)wifiScanRSSI[i]);
                M5.Display.setCursor(SCREEN_W - 60, ry + 10);
                M5.Display.print(rssiLabel);
            }

            // Scroll indicators
            if (wifiScrollTop > 0) {
                M5.Display.setTextColor(VC_GREEN, TFT_BLACK);
                M5.Display.setCursor(SCREEN_W / 2 - 6, y - 2);
                M5.Display.print("\x18");  // up arrow
            }
            if (endIdx < wifiScanCount) {
                M5.Display.setTextColor(VC_GREEN, TFT_BLACK);
                M5.Display.setCursor(SCREEN_W / 2 - 6, y + visibleRows * rowH);
                M5.Display.print("\x19");  // down arrow
            }

            // Count indicator
            M5.Display.setTextSize(1);
            M5.Display.setTextColor(TFT_DARKGREY, TFT_BLACK);
            M5.Display.setCursor(SCREEN_W - 50, UI_CONTENT_Y + 3);
            char countBuf[12];
            snprintf(countBuf, sizeof(countBuf), "%d/%d", wifiSelIdx + 1, wifiScanCount);
            M5.Display.print(countBuf);

            drawButtonRow("BACK", "SELECT", "NEXT",
                          VC_GREEN, TFT_GREEN, VC_GREEN);
        }

    } else if (wifiPhase == WPHASE_PASSWORD) {
        // ── Network name ────────────────────────────────────────────────
        M5.Display.setTextSize(1);
        M5.Display.setTextColor(VC_GREEN, TFT_BLACK);
        M5.Display.setCursor(4, UI_CONTENT_Y + 18);
        M5.Display.print("Network: ");
        M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
        String ssidDisp = wifiScanSSID[wifiSelIdx];
        if (ssidDisp.length() > 30) ssidDisp = ssidDisp.substring(0, 28) + "..";
        M5.Display.print(ssidDisp);

        // ── Password field ──────────────────────────────────────────────
        M5.Display.setTextColor(VC_GREEN, TFT_BLACK);
        M5.Display.setCursor(4, UI_CONTENT_Y + 34);
        M5.Display.print("Password:");
        // Show password with cursor
        M5.Display.setTextSize(1);
        M5.Display.setTextColor(TFT_CYAN, TFT_BLACK);
        M5.Display.setCursor(4, UI_CONTENT_Y + 48);
        // Show last 38 chars of password if it gets long
        String pwdShow = wifiPwdBuf;
        if (pwdShow.length() > 38) pwdShow = ".." + pwdShow.substring(pwdShow.length() - 36);
        M5.Display.print(pwdShow);
        // Blinking cursor
        if ((millis() / 500) % 2 == 0) {
            M5.Display.setTextColor(TFT_YELLOW, TFT_BLACK);
            M5.Display.print("_");
        }

        // ── Character picker ────────────────────────────────────────────
        // Show current char large, with neighbours on each side
        int cy = UI_CONTENT_Y + 72;
        M5.Display.fillRect(0, cy, SCREEN_W, 50, 0x0841u);  // subtle bg band

        // Neighbour chars (smaller, dimmed)
        M5.Display.setTextSize(2);
        for (int offset = -4; offset <= 4; offset++) {
            if (offset == 0) continue;
            int idx = ((int)wifiCharIdx + offset + WIFI_IDX_TOTAL) % WIFI_IDX_TOTAL;
            int px = SCREEN_W / 2 + offset * 28;
            if (px < 4 || px > SCREEN_W - 16) continue;
            M5.Display.setTextColor(TFT_DARKGREY, 0x0841u);
            M5.Display.setCursor(px - 6, cy + 16);
            if (idx == (int)WIFI_IDX_DONE) {
                M5.Display.setTextSize(1);
                M5.Display.print("OK");
                M5.Display.setTextSize(2);
            } else if (idx == (int)WIFI_IDX_DEL) {
                M5.Display.setTextSize(1);
                M5.Display.print("DEL");
                M5.Display.setTextSize(2);
            } else {
                M5.Display.print(WIFI_CHARS[idx]);
            }
        }

        // Current char — large and bright
        M5.Display.setTextSize(3);
        int cx = SCREEN_W / 2 - 9;
        if (wifiCharIdx == WIFI_IDX_DONE) {
            M5.Display.setTextColor(TFT_GREEN, 0x0841u);
            M5.Display.setCursor(cx - 18, cy + 10);
            M5.Display.setTextSize(2);
            M5.Display.print("[DONE]");
        } else if (wifiCharIdx == WIFI_IDX_DEL) {
            M5.Display.setTextColor(TFT_RED, 0x0841u);
            M5.Display.setCursor(cx - 15, cy + 10);
            M5.Display.setTextSize(2);
            M5.Display.print("[DEL]");
        } else {
            M5.Display.setTextColor(TFT_WHITE, 0x0841u);
            M5.Display.setCursor(cx, cy + 8);
            M5.Display.print(WIFI_CHARS[wifiCharIdx]);
        }

        // ── Hint line ───────────────────────────────────────────────────
        M5.Display.setTextSize(1);
        M5.Display.setTextColor(TFT_DARKGREY, TFT_BLACK);
        M5.Display.setCursor(10, UI_CONTENT_Y + 130);
        M5.Display.printf("Len: %d  |  PREV/NEXT char, ADD to append", wifiPwdBuf.length());

        // Category hint — show what region the char is in
        M5.Display.setCursor(10, UI_CONTENT_Y + 144);
        if (wifiCharIdx < 26) M5.Display.print("a-z lowercase");
        else if (wifiCharIdx < 52) M5.Display.print("A-Z uppercase");
        else if (wifiCharIdx < 62) M5.Display.print("0-9 digits");
        else if (wifiCharIdx < WIFI_CHAR_COUNT) M5.Display.print("symbols");
        else if (wifiCharIdx == WIFI_IDX_DONE) M5.Display.print("[DONE] = connect");
        else if (wifiCharIdx == WIFI_IDX_DEL) M5.Display.print("[DEL] = delete last char");

        drawButtonRow("PREV", "ADD", "NEXT",
                      VC_GREEN, TFT_GREEN, VC_GREEN);

    } else if (wifiPhase == WPHASE_CONNECTING) {
        M5.Display.setTextSize(2);
        M5.Display.setTextColor(TFT_CYAN, TFT_BLACK);
        M5.Display.setCursor(40, 70);
        M5.Display.print("Connecting...");
        M5.Display.setTextSize(1);
        M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
        M5.Display.setCursor(40, 100);
        M5.Display.print(wifiScanSSID[wifiSelIdx]);
        int dots = (millis() / 400) % 4;
        M5.Display.setTextColor(TFT_YELLOW, TFT_BLACK);
        M5.Display.setCursor(40, 120);
        for (int d = 0; d < dots; d++) M5.Display.print(". ");
        drawButtonRow("--", "--", "--",
                      TFT_DARKGREY, TFT_DARKGREY, TFT_DARKGREY);

    } else {  // WPHASE_RESULT
        M5.Display.setTextSize(2);
        uint16_t resCol = wifiResultOK ? TFT_GREEN : TFT_RED;
        M5.Display.setTextColor(resCol, TFT_BLACK);
        M5.Display.setCursor(20, 50);
        M5.Display.print(wifiResultOK ? "CONNECTED" : "FAILED");
        M5.Display.setTextSize(1);
        M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
        M5.Display.setCursor(20, 80);
        M5.Display.print(wifiResultMsg);
        if (wifiResultOK) {
            M5.Display.setTextColor(TFT_CYAN, TFT_BLACK);
            M5.Display.setCursor(20, 100);
            M5.Display.print("IP: ");
            M5.Display.print(WiFi.localIP().toString());
            M5.Display.setCursor(20, 116);
            M5.Display.printf("RSSI: %d dBm", wifiRSSI);
        }
        // Auto-return timer bar
        unsigned long elapsed = millis() - wifiResultMs;
        if (elapsed < TOOLS_RESULT_TIMEOUT_MS) {
            float frac = (float)elapsed / (float)TOOLS_RESULT_TIMEOUT_MS;
            int barW = (int)((1.0f - frac) * (SCREEN_W - 20));
            M5.Display.fillRect(10, 140, barW, 3, resCol);
        }
        drawButtonRow("SCAN", "--", "HOME",
                      VC_GREEN, TFT_DARKGREY, VC_GREEN);
    }
}

// =============================================================================
// WiFi picker: connect to selected network
// =============================================================================

void wifiPickerConnect() {
    String selectedSSID = wifiScanSSID[wifiSelIdx];
    String selectedPass = wifiScanOpen[wifiSelIdx] ? "" : wifiPwdBuf;

    // Save to globals + flash
    wifiSSID     = selectedSSID;
    wifiPassword = selectedPass;
    saveConfig();
    Serial.printf("[wifi-picker] Saved: SSID=%s\n", wifiSSID.c_str());

    // Attempt connection
    connectWiFi();

    if (wifiConnected) {
        wifiResultOK  = true;
        wifiResultMsg = "Saved: " + wifiSSID;
        initNTP();
    } else {
        wifiResultOK  = false;
        wifiResultMsg = "Could not connect to " + selectedSSID;
    }
    wifiPhase    = WPHASE_RESULT;
    wifiResultMs = millis();
}

// =============================================================================
// WiFi picker: button handling
// =============================================================================

// Exit the WiFi picker to dest screen.  If WiFi was up when we entered but
// is now down (scan tore it down and no new connection was made), reset
// lastWifiCheckMs so checkWiFi() retries immediately on the next loop tick
// instead of waiting up to WIFI_RETRY_MS.
static void exitWifiPicker(UIScreen dest) {
    if (wifiWasConnected && !wifiConnected) {
        lastWifiCheckMs = 0;
    }
    currentScreen = dest;
}

void handleWifiButtons() {
    switch (wifiPhase) {
    case WPHASE_SCANNING:
        // No input while scanning
        break;

    case WPHASE_SELECT:
        if (wifiScanCount == 0) {
            // No networks: SCAN or BACK
            if (M5.BtnA.wasPressed()) {
                wifiPhase = WPHASE_SCANNING;  // re-scan triggered in loop()
            }
            if (M5.BtnC.wasPressed()) {
                exitWifiPicker(SCREEN_CONFIG);
            }
        } else {
            if (M5.BtnA.wasPressed()) {
                // BACK — return to config without connecting
                exitWifiPicker(SCREEN_CONFIG);
            }
            if (M5.BtnC.wasPressed()) {
                // NEXT (wraps, so all networks reachable without PREV)
                wifiSelIdx = (wifiSelIdx + 1) % wifiScanCount;
            }
            if (M5.BtnB.wasPressed()) {
                // SELECT — open networks skip password
                if (wifiScanOpen[wifiSelIdx]) {
                    wifiPwdBuf = "";
                    wifiPhase  = WPHASE_CONNECTING;
                } else {
                    wifiPwdBuf  = "";
                    wifiCharIdx = 0;
                    wifiPhase   = WPHASE_PASSWORD;
                }
            }
        }
        break;

    case WPHASE_PASSWORD:
        if (M5.BtnA.wasPressed()) {
            // PREV char
            if (wifiCharIdx == 0) wifiCharIdx = WIFI_IDX_TOTAL - 1;
            else wifiCharIdx--;
        }
        if (M5.BtnC.wasPressed()) {
            // NEXT char
            wifiCharIdx = (wifiCharIdx + 1) % WIFI_IDX_TOTAL;
        }
        if (M5.BtnB.wasPressed()) {
            // ADD / action
            if (wifiCharIdx == WIFI_IDX_DONE) {
                // Done — connect
                wifiPhase = WPHASE_CONNECTING;
            } else if (wifiCharIdx == WIFI_IDX_DEL) {
                // Delete last char
                if (wifiPwdBuf.length() > 0)
                    wifiPwdBuf.remove(wifiPwdBuf.length() - 1);
            } else {
                // Append character
                if (wifiPwdBuf.length() < 63)  // WPA max = 63 chars
                    wifiPwdBuf += WIFI_CHARS[wifiCharIdx];
            }
        }
        break;

    case WPHASE_CONNECTING:
        // No input while connecting
        break;

    case WPHASE_RESULT:
        if (M5.BtnA.wasPressed()) {
            // SCAN again
            wifiPhase = WPHASE_SCANNING;
        }
        if (M5.BtnC.wasPressed()) {
            exitWifiPicker(SCREEN_HOME);
        }
        if ((millis() - wifiResultMs) > TOOLS_RESULT_TIMEOUT_MS) {
            exitWifiPicker(SCREEN_CONFIG);
        }
        break;
    }
}

// =============================================================================
// UI: Full UI redraw dispatcher
// =============================================================================

void updateDisplay() {
    M5.Display.startWrite();
    drawHazard(0, 0, SCREEN_W, UI_TOP_H);
    switch (currentScreen) {
        case SCREEN_HOME:     drawHomeScreen();     break;
        case SCREEN_STATUS:   drawStatusScreen();   break;
        case SCREEN_CONFIG:   drawConfigScreen();   break;
        case SCREEN_TOOLS:    drawToolsScreen();    break;
        case SCREEN_DURATION: drawDurationScreen(); break;
        case SCREEN_WIFI:     drawWifiScreen();     break;
    }
    M5.Display.endWrite();
}

// =============================================================================
// Tools: individual tool implementations
// =============================================================================

void toolsRunWifiTest() {
    connectWiFi();
    if (wifiConnected) {
        toolsResultOK    = true;
        toolsResultLine1 = "Connected: " + wifiSSID;
        toolsResultLine2 = "IP: " + WiFi.localIP().toString() +
                           "  RSSI: " + String(wifiRSSI) + " dBm";
    } else {
        toolsResultOK    = false;
        toolsResultLine1 = "FAILED \x7e check credentials";
        toolsResultLine2 = "Use: wifi <ssid> <password>";
    }
}

void toolsRunOtaTest() {
    if (!wifiConnected) {
        toolsResultOK    = false;
        toolsResultLine1 = "No WiFi \x7e connect first";
        toolsResultLine2 = "";
        return;
    }
    HTTPClient http;
    http.setTimeout(10000);
    http.begin(OTA_VERSION_URL);
    int code = http.GET();
    if (code == 200) {
        String remote = http.getString();
        remote.trim();
        http.end();
        toolsResultOK    = true;
        toolsResultLine1 = "Local: " FIRMWARE_VERSION "  Remote: " + remote;
        toolsResultLine2 = (remote == FIRMWARE_VERSION) ? "Firmware is current"
                                                        : "Update available!";
    } else {
        http.end();
        toolsResultOK    = false;
        toolsResultLine1 = "HTTP " + String(code);
        toolsResultLine2 = OTA_VERSION_URL;
    }
}

void toolsRunPostTest() {
    if (!wifiConnected) {
        toolsResultOK    = false;
        toolsResultLine1 = "No WiFi \x7e connect first";
        toolsResultLine2 = "";
        return;
    }
    const size_t SILENCE_SAMPLES = (size_t)SAMPLE_RATE;   // 1 second
    int16_t* silenceBuf = (int16_t*)ps_malloc(SILENCE_SAMPLES * 2u);
    if (!silenceBuf) {
        toolsResultOK    = false;
        toolsResultLine1 = "PSRAM alloc failed";
        toolsResultLine2 = String(ESP.getFreePsram()) + " bytes free";
        return;
    }
    memset(silenceBuf, 0, SILENCE_SAMPLES * 2u);
    bool ok = buildAndUploadVConCore(silenceBuf, SILENCE_SAMPLES);
    free(silenceBuf);
    toolsResultOK    = ok;
    toolsResultLine1 = ok ? "POST succeeded" : "POST failed";
    String uuidShort = lastVconUUID.length() >= 8
                       ? lastVconUUID.substring(lastVconUUID.length() - 8) : "--";
    toolsResultLine2 = "HTTP " + String((int)uploadTaskHttpCode) +
                       "  uuid: ..." + uuidShort;
    lastHttpCode = (int)uploadTaskHttpCode;
}

void toolsShowSD() {
    if (!sdReady) {
        toolsResultOK    = false;
        toolsResultLine1 = "No SD card detected";
        toolsResultLine2 = "Format FAT32, reinsert, restart";
        return;
    }
    uint64_t totalMB = SD.totalBytes() / (1024ULL * 1024ULL);
    uint64_t usedMB  = SD.usedBytes()  / (1024ULL * 1024ULL);
    uint64_t freeMB  = totalMB - usedMB;
    toolsResultOK    = true;
    toolsResultLine1 = String((uint32_t)totalMB) + "MB total  " +
                       String((uint32_t)usedMB)  + "MB used";
    toolsResultLine2 = String((uint32_t)freeMB)  + "MB free  saved:" +
                       String(sdSaved);
}

void toolsRunSelected() {
    switch ((ToolsItem)toolsSelectedItem) {
        case TOOL_WIFI_TEST: toolsRunWifiTest(); break;
        case TOOL_OTA_TEST:  toolsRunOtaTest();  break;
        case TOOL_POST_TEST: toolsRunPostTest(); break;
        case TOOL_SD_SHOW:   toolsShowSD();      break;
        default: break;
    }
    toolsPhase    = TPHASE_RESULT;
    toolsResultMs = millis();
}

// =============================================================================
// Button handling
// =============================================================================

void handleToolsButtons() {
    switch (toolsPhase) {
    case TPHASE_SELECT:
        if (M5.BtnA.wasPressed()) {
            toolsSelectedItem = (toolsSelectedItem + TOOL_COUNT - 1) % TOOL_COUNT;
        }
        if (M5.BtnB.wasPressed()) {
            if (appState == STATE_RECORDING || appState == STATE_CONTINUOUS) {
                toolsResultOK    = false;
                toolsResultLine1 = "Stop recording first";
                toolsResultLine2 = "Press HOME STOP then return";
                toolsPhase    = TPHASE_RESULT;
                toolsResultMs = millis();
            } else if (toolsSelectedItem == TOOL_RESTART) {
                toolsPhase = TPHASE_CONFIRM;
            } else {
                toolsPhase = TPHASE_RUNNING;
                // toolsRunSelected() is called from loop() on next iteration
            }
        }
        if (M5.BtnC.wasPressed()) {
            currentScreen = SCREEN_HOME;
        }
        break;

    case TPHASE_CONFIRM:
        if (M5.BtnB.wasPressed()) {
            Serial.println("[tools] User-initiated restart");
            delay(200);
            ESP.restart();
        }
        if (M5.BtnC.wasPressed()) {
            toolsPhase    = TPHASE_SELECT;
            currentScreen = SCREEN_HOME;
        }
        break;

    case TPHASE_RUNNING:
        // no input while running
        break;

    case TPHASE_RESULT:
        if (M5.BtnB.wasPressed()) toolsPhase = TPHASE_SELECT;
        if (M5.BtnC.wasPressed()) {
            toolsPhase    = TPHASE_SELECT;
            currentScreen = SCREEN_HOME;
        }
        if ((millis() - toolsResultMs) > TOOLS_RESULT_TIMEOUT_MS) {
            toolsPhase = TPHASE_SELECT;
        }
        break;
    }
}

void handleButtons() {
    bool anyPressed = M5.BtnA.wasPressed() || M5.BtnB.wasPressed() || M5.BtnC.wasPressed();
    if (anyPressed) lastButtonMs = millis();

    // Inactivity auto-return to HOME
    if (currentScreen != SCREEN_HOME &&
        (millis() - lastButtonMs) > INACTIVITY_TIMEOUT_MS) {
        currentScreen = SCREEN_HOME;
        return;
    }

    bool idle   = (appState == STATE_IDLE    ||
                   appState == STATE_SUCCESS ||
                   appState == STATE_ERROR);
    bool activeRec = (appState == STATE_RECORDING || appState == STATE_CONTINUOUS);

    switch (currentScreen) {

    case SCREEN_HOME:
        if (idle) {
            if (M5.BtnA.wasPressed()) {
                // RUN — same logic as old BtnA handler
                checkWiFi();
                if (!audioBuffer && !allocateAudioBuffer()) {
                    lastStatus    = "PSRAM alloc failed!";
                    appState      = STATE_ERROR;
                    stateChangeMs = millis();
                } else if (recordDurationSec > CONT_MAX_DURATION_SEC) {
                    Serial.printf("[audio] Duration %u s > %u s — single-shot\n",
                                  recordDurationSec, CONT_MAX_DURATION_SEC);
                    continuousMode = false;
                    stopContinuous = false;
                    startRecording();
                } else if (!allocateContinuousBuffers()) {
                    lastStatus    = "PSRAM bufB alloc fail";
                    appState      = STATE_ERROR;
                    stateChangeMs = millis();
                } else {
                    continuousMode   = true;
                    stopContinuous   = false;
                    continuousChunks = 0;
                    recordBuf        = audioBufA;
                    startRecording();
                }
            }
            if (M5.BtnB.wasPressed()) currentScreen = SCREEN_CONFIG;
            if (M5.BtnC.wasPressed()) currentScreen = SCREEN_TOOLS;
        } else if (activeRec) {
            if (M5.BtnB.wasPressed()) {
                // STOP — same logic as old BtnB handler
                if (appState == STATE_CONTINUOUS) {
                    stopContinuous = true;
                    lastStatus = "Stopping after this chunk...";
                } else if (appState == STATE_RECORDING) {
                    M5.Mic.end();
                    isRecording = false;
                    if (audioBufferIndex > 0) {
                        appState   = STATE_ENCODING;
                        lastStatus = "Stopped early \x7e encoding...";
                    } else {
                        appState   = STATE_IDLE;
                        lastStatus = "Recording cancelled";
                    }
                }
            }
            if (M5.BtnC.wasPressed()) currentScreen = SCREEN_STATUS;
        } else {
            // ENCODING / UPLOADING
            if (M5.BtnC.wasPressed()) currentScreen = SCREEN_STATUS;
        }
        break;

    case SCREEN_STATUS:
        if (M5.BtnA.wasPressed()) currentScreen = SCREEN_HOME;
        break;

    case SCREEN_CONFIG:
        if (M5.BtnA.wasPressed()) currentScreen = SCREEN_HOME;
        if (M5.BtnB.wasPressed()) {
            durEditPending = recordDurationSec;   // seed picker with live value
            currentScreen  = SCREEN_DURATION;
        }
        if (M5.BtnC.wasPressed()) {
            wifiWasConnected = wifiConnected;
            wifiPhase        = WPHASE_SCANNING;
            currentScreen    = SCREEN_WIFI;
        }
        break;

    case SCREEN_DURATION:
        if (M5.BtnA.wasPressed()) {
            // LESS — decrease by 5 s, clamp at minimum
            if (durEditPending > MIN_RECORD_DURATION_SEC) {
                uint32_t next = durEditPending - 5;
                durEditPending = (next < MIN_RECORD_DURATION_SEC)
                                 ? MIN_RECORD_DURATION_SEC : next;
            }
        }
        if (M5.BtnC.wasPressed()) {
            // MORE — increase by 5 s, clamp at maximum
            if (durEditPending < MAX_RECORD_DURATION_SEC) {
                uint32_t next = durEditPending + 5;
                durEditPending = (next > MAX_RECORD_DURATION_SEC)
                                 ? MAX_RECORD_DURATION_SEC : next;
            }
        }
        if (M5.BtnB.wasPressed()) {
            // SAVE — commit and free old buffers so they reallocate at new size
            if (appState == STATE_RECORDING || appState == STATE_CONTINUOUS ||
                appState == STATE_ENCODING  || appState == STATE_UPLOADING) {
                // Can't change mid-recording — silently ignore (screen shows old value)
            } else {
                recordDurationSec = durEditPending;
                freeAudioBuffers();
                saveConfig();
                Serial.printf("[dur] Duration set to %u s (%s)\n",
                              recordDurationSec,
                              recordDurationSec <= CONT_MAX_DURATION_SEC
                              ? "continuous" : "single-shot");
            }
            currentScreen = SCREEN_CONFIG;
        }
        break;

    case SCREEN_TOOLS:
        handleToolsButtons();
        break;

    case SCREEN_WIFI:
        handleWifiButtons();
        break;
    }
}

// =============================================================================
// Serial Command Handler
// =============================================================================

void handleSerialCommand(const String& cmd) {
    if (cmd.startsWith("wifi ")) {
        String params = cmd.substring(5);
        // Split on the LAST space so SSIDs with spaces work:
        //   wifi Barnhill Tavern meteormeteor  →  ssid="Barnhill Tavern" pass="meteormeteor"
        int sp = params.lastIndexOf(' ');
        if (sp > 0) {
            wifiSSID     = params.substring(0, sp);
            wifiPassword = params.substring(sp + 1);
            wifiPassword.trim();
            saveConfig();
            Serial.printf("[cmd] WiFi updated: %s\n", wifiSSID.c_str());
            WiFi.disconnect();
            connectWiFi();
            initNTP();
        } else {
            Serial.println("Usage: wifi <ssid> <password>");
        }
    } else if (cmd.startsWith("url ")) {
        postURL = cmd.substring(4);
        postURL.trim();
        saveConfig();
        Serial.printf("[cmd] POST URL set: %s\n", postURL.c_str());
    } else if (cmd.startsWith("token ")) {
        deviceToken = cmd.substring(6);
        deviceToken.trim();
        saveConfig();
        Serial.printf("[cmd] Device token set: %s\n", deviceToken.c_str());
    } else if (cmd == "token") {
        // 'token' with no value clears it (fall back to MAC routing)
        deviceToken = "";
        saveConfig();
        Serial.println("[cmd] Device token cleared — will use MAC routing");
    } else if (cmd.startsWith("dur ") || cmd == "dur") {
        if (cmd == "dur") {
            // 'dur' with no argument prints current value
            Serial.printf("[cmd] Recording duration: %u s (%s mode)\n",
                          recordDurationSec,
                          recordDurationSec <= CONT_MAX_DURATION_SEC ? "continuous" : "single-shot");
        } else {
            // Reject change while recording
            if (appState == STATE_RECORDING || appState == STATE_CONTINUOUS ||
                appState == STATE_ENCODING  || appState == STATE_UPLOADING) {
                Serial.println("[cmd] Cannot change duration while recording — stop first");
            } else {
                uint32_t newDur = (uint32_t)cmd.substring(4).toInt();
                if (newDur < MIN_RECORD_DURATION_SEC) {
                    Serial.printf("[cmd] Minimum duration is %u s\n", MIN_RECORD_DURATION_SEC);
                } else if (newDur > MAX_RECORD_DURATION_SEC) {
                    Serial.printf("[cmd] Maximum duration is %u s\n", MAX_RECORD_DURATION_SEC);
                } else {
                    recordDurationSec = newDur;
                    saveConfig();
                    // Free existing buffers — they'll be reallocated at the new size on next RUN
                    freeAudioBuffers();
                    Serial.printf("[cmd] Duration set to %u s", recordDurationSec);
                    if (recordDurationSec > CONT_MAX_DURATION_SEC) {
                        Serial.printf(" (> %u s — continuous mode disabled, using single-shot)\n",
                                      CONT_MAX_DURATION_SEC);
                    } else {
                        Serial.println(" (continuous mode available)");
                    }
                    Serial.println("[cmd] Buffers freed — will reallocate on next RUN press");
                }
            }
        }
    } else if (cmd == "status") {
        Serial.println("\n=== vCon Recorder Status ===");
        Serial.printf("Firmware:    %s\n", FIRMWARE_VERSION);
        Serial.printf("Device ID:   %s\n", deviceID.c_str());
        Serial.printf("MAC Address: %s\n", WiFi.macAddress().c_str());
        Serial.printf("Rec Duration:%u s (%s mode, range %u-%u s)\n",
                      recordDurationSec,
                      recordDurationSec <= CONT_MAX_DURATION_SEC ? "continuous" : "single-shot",
                      MIN_RECORD_DURATION_SEC, MAX_RECORD_DURATION_SEC);
        Serial.printf("Device Token:%s\n",
                      deviceToken.length() > 0 ? deviceToken.c_str() : "(none — using MAC routing)");
        Serial.printf("WiFi SSID:   %s\n", wifiSSID.c_str());
        Serial.printf("WiFi Status: %s\n", wifiConnected ? "Connected" : "Disconnected");
        if (wifiConnected) {
            Serial.printf("IP Address:  %s\n", WiFi.localIP().toString().c_str());
            Serial.printf("RSSI:        %d dBm\n", wifiRSSI);
        }
        Serial.printf("NTP Synced:  %s\n", ntpSynced ? "Yes" : "No");
        Serial.printf("POST URL:    %s\n", postURL.c_str());
        Serial.printf("Total OK:    %u\n", totalUploads);
        Serial.printf("Total Fail:  %u\n", failedUploads);
        Serial.printf("Free Heap:   %u bytes\n", ESP.getFreeHeap());
        Serial.printf("Free PSRAM:  %u bytes\n", ESP.getFreePsram());
        Serial.printf("App State:   %d\n", (int)appState);
        Serial.println("============================\n");
    } else if (cmd == "scan") {
        Serial.println("[scan] Scanning for WiFi networks...");
        WiFi.disconnect(true); WiFi.mode(WIFI_OFF); delay(100);
        WiFi.mode(WIFI_STA); delay(100);
        int n = WiFi.scanNetworks();
        if (n == 0) {
            Serial.println("[scan] No networks found");
        } else {
            for (int i = 0; i < n; i++) {
                Serial.printf("[scan] %2d: RSSI %4d  %-32s  %s\n",
                              i + 1, WiFi.RSSI(i), WiFi.SSID(i).c_str(),
                              WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? "OPEN" : "SECURED");
            }
        }
        WiFi.scanDelete();
    } else if (cmd == "sd") {
        if (!sdReady) {
            Serial.println("[sd] No SD card (or not FAT32 formatted)");
        } else {
            Serial.printf("[sd] Card: %s\n", sdCardInfo.c_str());
            Serial.printf("[sd] Total: %llu MB  Used: %llu MB\n",
                          SD.totalBytes() / (1024ULL * 1024ULL),
                          SD.usedBytes()  / (1024ULL * 1024ULL));
            Serial.printf("[sd] Recordings saved this session: %u\n", sdSaved);
        }
    } else if (cmd == "restart") {
        Serial.println("[cmd] Restarting...");
        delay(500);
        ESP.restart();
    } else if (cmd == "help") {
        Serial.println("\n=== M5Stack Core2 vCon Recorder ===");
        Serial.println("wifi  <ssid> <password>  Set WiFi credentials");
        Serial.println("url   <post_url>          Set vCon POST URL");
        Serial.println("token <dvt_xxx>           Set portal device token (token routing)");
        Serial.println("token                     Clear token (use MAC routing)");
        Serial.printf( "dur   <10-%u>           Set recording duration in seconds\n",
                       MAX_RECORD_DURATION_SEC);
        Serial.println("dur                       Show current recording duration");
        Serial.println("status                    Show current config");
        Serial.println("sd                        Show SD card status");
        Serial.println("restart                   Reboot device");
        Serial.println("help                      This message");
        Serial.println("=====================================\n");
    } else if (cmd.length() > 0) {
        Serial.printf("[cmd] Unknown: %s  (type 'help')\n", cmd.c_str());
    }
}

void checkSerial() {
    static String buf = "";
    while (Serial.available()) {
        char c = Serial.read();
        if (c == '\n' || c == '\r') {
            buf.trim();
            if (buf.length()) handleSerialCommand(buf);
            buf = "";
        } else {
            buf += c;
        }
    }
}

// =============================================================================
// Logo splash screen
// =============================================================================

void showLogoSplash() {
    M5.Display.fillScreen(TFT_BLACK);

    // Centre the logo, shifted up slightly to leave room for the device ID
    int x = (SCREEN_W - LOGO_W) / 2;
    int y = (SCREEN_H - LOGO_H) / 2 - 14;
    M5.Display.pushImage(x, y, LOGO_W, LOGO_H, LOGO_DATA);

    // Show the unique device identifier below the logo
    // Size-2 font = 12×16 px per character
    int idPixelW = deviceID.length() * 12;
    M5.Display.setTextSize(2);
    M5.Display.setTextColor(VC_GREEN, TFT_BLACK);
    M5.Display.setCursor((SCREEN_W - idPixelW) / 2, y + LOGO_H + 10);
    M5.Display.print(deviceID);

    // Also show a muted tagline
    M5.Display.setTextSize(1);
    M5.Display.setTextColor(0x4A49u, TFT_BLACK);   // dim grey
    M5.Display.setCursor((SCREEN_W - 96) / 2, y + LOGO_H + 32);
    M5.Display.print("vCon Recorder v" FIRMWARE_VERSION);

    delay(10000);   // 10 seconds
}

// =============================================================================
// setup()
// =============================================================================

void setup() {
    auto cfg = M5.config();
    cfg.internal_mic = true;
    cfg.external_spk = false;
    M5.begin(cfg);

    Serial.begin(115200);
    delay(300);
    Serial.println("\n\n=== M5Stack Core2 vCon Recorder (VConic) ===");
    Serial.println("Type 'help' for available commands.\n");

    M5.Display.setBrightness(160);

    // ---- Derive device ID from MAC address (available before WiFi connect) ----
    // Format: "VC-XXXXXX" where XXXXXX = last 3 bytes of the station MAC.
    // This gives a short, human-readable identifier unique to each device.
    {
        uint8_t mac[6];
        esp_read_mac(mac, ESP_MAC_WIFI_STA);
        char devIdBuf[10];
        snprintf(devIdBuf, sizeof(devIdBuf), "VC-%02X%02X%02X",
                 mac[3], mac[4], mac[5]);
        deviceID = String(devIdBuf);
        Serial.printf("[init] Device ID: %s\n", deviceID.c_str());
    }

    // ---- Logo splash (shows device ID for 10 seconds) ----
    showLogoSplash();

    // ---- Transition to dark initialisation screen ----
    M5.Display.fillScreen(TFT_BLACK);
    M5.Display.setTextSize(1);
    M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
    M5.Display.setCursor(40, 120);
    M5.Display.print("Initializing...");

    // PSRAM check — Core2 requires it for the 960 KB audio buffer
    if (!psramFound()) {
        M5.Display.fillScreen(TFT_RED);
        M5.Display.setTextColor(TFT_WHITE, TFT_RED);
        M5.Display.setCursor(20, 80);
        M5.Display.setTextSize(2);
        M5.Display.print("FATAL: No PSRAM");
        M5.Display.setCursor(20, 110);
        M5.Display.print("This firmware");
        M5.Display.setCursor(20, 130);
        M5.Display.print("requires Core2");
        while (true) delay(1000);
    }
    Serial.printf("[init] PSRAM: %u bytes total, %u bytes free\n",
                  ESP.getPsramSize(), ESP.getFreePsram());

    loadConfig();
    initSD();

    // Allocate audio buffer early — keeps a contiguous PSRAM block reserved
    if (!allocateAudioBuffer()) {
        M5.Display.fillScreen(TFT_RED);
        M5.Display.setTextColor(TFT_WHITE, TFT_RED);
        M5.Display.setCursor(20, 100);
        M5.Display.setTextSize(2);
        M5.Display.print("PSRAM alloc fail");
        while (true) delay(1000);
    }

    // Verify microphone is available
    if (!M5.Mic.isEnabled()) {
        Serial.println("[init] WARNING: Microphone not available");
        lastStatus = "Microphone unavailable!";
    } else {
        Serial.println("[init] Microphone OK");
    }

    M5.Display.setCursor(40, 130);
    M5.Display.setTextSize(1);
    M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
    M5.Display.print("Connecting WiFi...");

    connectWiFi();

    // OTA check — only when WiFi is up; blocks until done or skipped
    if (wifiConnected) {
        M5.Display.setCursor(40, 145);
        M5.Display.print("Checking for update...");
        checkForOTA(Serial);   // reboots here if a new firmware is flashed
        M5.Display.fillRect(40, 145, 240, 10, TFT_BLACK);  // clear line
    }

    initNTP();

    updateDisplay();
    lastButtonMs = millis();

    Serial.println("[init] Ready. Press BtnA to record.");
}

// =============================================================================
// loop()
// =============================================================================

void loop() {
    yield();
    M5.update();
    checkSerial();

    unsigned long now = millis();

    // ---- Continuous mode tick -------------------------------------------
    if (appState == STATE_CONTINUOUS && isRecording) {
        recordAudioChunk();
        swapAndContinue();
    }

    // After STOP: wait for the last upload task to finish, then go idle
    if (appState == STATE_UPLOADING && !uploadBusy) {
        lastHttpCode  = (int)uploadTaskHttpCode;
        bool ok = (uploadTaskState == UTS_DONE_OK);
        lastStatus    = ok ? ("Finished. " + String((uint32_t)continuousChunks) + " chunks sent")
                           : "Last chunk failed: HTTP " + String(lastHttpCode);
        appState      = ok ? STATE_SUCCESS : STATE_ERROR;
        stateChangeMs = millis();
        continuousChunks = 0;   // reset for next session
        Serial.printf("[cont] Session ended. total OK uploads: %u\n", totalUploads);
    }

    // ---- Normal single-shot recording -----------------------------------
    if (appState == STATE_RECORDING) {
        recordAudioChunk();
    }
    if (appState == STATE_ENCODING) {
        checkWiFi();
        buildAndUploadVCon();
    }

    // ---- TOOLS execution — run selected tool when phase is RUNNING ----------
    if (currentScreen == SCREEN_TOOLS && toolsPhase == TPHASE_RUNNING) {
        toolsRunSelected();
    }

    // ---- WIFI picker execution — blocking scan / connect phases ----------
    if (currentScreen == SCREEN_WIFI) {
        if (wifiPhase == WPHASE_SCANNING) {
            updateDisplay();   // show "Scanning..." before blocking call
            wifiRunScan();
            wifiPhase = WPHASE_SELECT;
        } else if (wifiPhase == WPHASE_CONNECTING) {
            updateDisplay();   // show "Connecting..." before blocking call
            wifiPickerConnect();
        }
    }

    // ---- Button handling ------------------------------------------------
    handleButtons();

    // ---- Auto-return from SUCCESS/ERROR after 8 seconds ----------------
    if ((appState == STATE_SUCCESS || appState == STATE_ERROR) &&
        stateChangeMs > 0 && (now - stateChangeMs) > 8000) {
        appState      = STATE_IDLE;
        stateChangeMs = 0;
        lastStatus    = "Press RUN to begin";
    }

    // ---- Periodic WiFi check (when idle) --------------------------------
    if (appState == STATE_IDLE && (now - lastWifiCheckMs) > WIFI_RETRY_MS) {
        checkWiFi();
    }

    // ---- Update lastStatus from upload task state (continuous mode) -----
    if (appState == STATE_CONTINUOUS && uploadBusy) {
        switch (uploadTaskState) {
            case UTS_ENCODING:  lastStatus = "Chunk " + String((uint32_t)continuousChunks + 1) + ": encoding..."; break;
            case UTS_SD:        lastStatus = "Chunk " + String((uint32_t)continuousChunks + 1) + ": saving SD..."; break;
            case UTS_UPLOADING: lastStatus = "Chunk " + String((uint32_t)continuousChunks + 1) + ": posting..."; break;
            case UTS_RETRY:     lastStatus = "Chunk " + String((uint32_t)continuousChunks + 1) + ": retry " + String(uploadRetryNum) + "..."; break;
            default: break;
        }
    }

    // ---- Periodic display refresh ----
    if (now - lastDisplayMs >= DISPLAY_INTERVAL_MS) {
        lastDisplayMs = now;
        updateDisplay();
    }

    delay(1);
}
