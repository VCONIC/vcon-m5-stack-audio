
// =============================================================================
// M5Stack vCon Audio Recorder  (VConic)
// =============================================================================
// Supports: M5Stack Core2, M5Stack CoreS3
//
// Records audio from the built-in microphone and POSTs a spec-compliant
// vCon JSON (IETF draft-ietf-vcon-vcon-core) to a configured endpoint.
// Audio is embedded inline as base64url-encoded WAV.
// On CoreS3 a camera thumbnail is captured and included as an attachment.
//
// Serial Commands (115200 baud):
//   wifi <ssid> <password>  — Set WiFi credentials (saved to flash)
//   url  <post_url>         — Set vCon POST URL   (saved to flash)
//   status                  — Show current configuration
//   restart                 — Restart device
//   help                    — Show available commands
//
// Buttons (BtnA / BtnB / BtnC):
//   A = RUN     Start recording, then auto-encode + upload
//   B = STOP    Stop recording early (encodes + uploads partial audio)
//   C = CONFIG  Show configuration screen
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
#include "hardware.h"
#include "shine_mp3.h"
#include "logo.h"
#include "ota.h"

#if HAS_CAMERA
#include "esp_camera.h"
#endif

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

// Camera snapshot (CoreS3 only — PSRAM)
#if HAS_CAMERA
bool     cameraReady = false;
uint8_t* g_snapBuf   = nullptr;   // JPEG snapshot taken at recording start
size_t   g_snapLen   = 0;
#endif

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

// =============================================================================
// SD-streaming mode — record directly to SD card, bypassing PSRAM buffers.
// =============================================================================
// Activated when recordDurationSec > CONT_MAX_DURATION_SEC or forced via
// serial command.  Audio is written to SD as PCM WAV (or MP3 when enabled)
// in real time.  Upload reads back from SD using VConStream (Phase 3).

bool     sdStreamMode    = false;   // true → recording to SD (vs PSRAM)
bool     sdStreamForce   = false;   // user override via serial "sdstream on"
bool     useMp3          = false;   // runtime toggle: MP3 vs WAV for SD-stream
File     sdRecFile;                 // open SD file during SD-stream recording
char     sdRecPath[120];            // full path to current recording file
char     sdRecUUID[37];             // UUID for current SD-stream recording
uint32_t sdRecSamples    = 0;       // PCM samples written so far
uint32_t sdRecBytes      = 0;       // bytes written to SD file (differs for MP3)

#if ENABLE_MP3
static shine_t  mp3Enc          = nullptr;
static int      mp3SamplesPerFrame = 0;  // set at runtime via shine_samples_per_pass()
static int16_t  mp3AccumBuf[SHINE_MAX_SAMPLES]; // frame accumulator (max 1152)
static uint16_t mp3AccumCount   = 0;
#endif

// Audio level history — 16 bars for the VU-meter display
#define LEVEL_BARS 16
int      levelHistory[LEVEL_BARS] = {0};
int      levelIdx = 0;

// Display
unsigned long lastDisplayMs = 0;
const unsigned long DISPLAY_INTERVAL_MS = 300;

// =============================================================================
// Touch-screen button emulation (CoreS3)
// =============================================================================
// CoreS3 has no physical buttons below the screen.  We map taps in the
// on-screen button row (y >= UI_BTN_Y) to virtual BtnA / BtnB / BtnC.
// State is computed once per frame in updateTouchButtons() before any
// button-check code runs.  On Core2 this compiles away to nothing.
// =============================================================================
#if TOUCH_SCREEN_BUTTONS
static bool g_touchA = false, g_touchB = false, g_touchC = false;

void updateTouchButtons() {
    g_touchA = g_touchB = g_touchC = false;
    auto t = M5.Touch.getDetail();
    if (t.wasPressed() && t.y >= UI_BTN_Y) {
        int zone = t.x * 3 / SCREEN_W;
        if (zone == 0)      g_touchA = true;
        else if (zone == 1) g_touchB = true;
        else                g_touchC = true;
    }
}
inline bool btnAPressed() { return M5.BtnA.wasPressed() || g_touchA; }
inline bool btnBPressed() { return M5.BtnB.wasPressed() || g_touchB; }
inline bool btnCPressed() { return M5.BtnC.wasPressed() || g_touchC; }
#else
inline void updateTouchButtons() {}
inline bool btnAPressed() { return M5.BtnA.wasPressed(); }
inline bool btnBPressed() { return M5.BtnB.wasPressed(); }
inline bool btnCPressed() { return M5.BtnC.wasPressed(); }
#endif

// Full-screen off-screen canvas for flicker-free rendering.
// All draw functions paint into this sprite; updateDisplay() pushes the
// completed frame to the physical display in one transfer so intermediate
// black-clear states are never visible on screen.
static M5Canvas            frameCanvas(&M5.Display);
static lgfx::LovyanGFX*   gfx = &M5.Display; // normally &frameCanvas during updateDisplay()

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

static void exitWifiPicker(UIScreen dest);

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
#if ENABLE_MP3
    useMp3            = prefs.getBool("use_mp3",  false);
#endif
    prefs.end();
    // Clamp in case a previously stored value is out of range
    if (recordDurationSec < MIN_RECORD_DURATION_SEC) recordDurationSec = MIN_RECORD_DURATION_SEC;
    if (recordDurationSec > MAX_RECORD_DURATION_SEC) recordDurationSec = MAX_RECORD_DURATION_SEC;
    Serial.printf("[config] SSID=%s\n[config] URL=%s\n",
                  wifiSSID.c_str(), postURL.c_str());
    Serial.printf("[config] Duration=%u s\n", recordDurationSec);
#if ENABLE_MP3
    Serial.printf("[config] MP3=%s\n", useMp3 ? "on" : "off");
#endif
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
#if ENABLE_MP3
    prefs.putBool("use_mp3",  useMp3);
#endif
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
    WiFi.setAutoReconnect(true);
    delay(100);
    // Static IP — only applied when WIFI_STATIC_IP_ENABLE is 1 in config.h.
    // Set to 0 to use DHCP instead.
#if WIFI_STATIC_IP_ENABLE
    {
        IPAddress ip, gw, sn, dns;
        ip.fromString(WIFI_STATIC_IP);
        gw.fromString(WIFI_STATIC_GW);
        sn.fromString(WIFI_STATIC_SUBNET);
        dns.fromString(WIFI_STATIC_DNS);
        WiFi.config(ip, gw, sn, dns);
        Serial.printf("[wifi] Using static IP %s\n", WIFI_STATIC_IP);
    }
#endif
    wl_status_t rc = WiFi.begin(wifiSSID.c_str(), wifiPassword.c_str());
    Serial.printf("[wifi] begin() returned %d\n", rc);
    for (int i = 0; i < 30 && WiFi.status() != WL_CONNECTED; i++) {
        delay(500);
        if (i % 5 == 0) Serial.printf("[wifi] poll %d status=%d\n", i, WiFi.status());
    }
    wifiConnected = (WiFi.status() == WL_CONNECTED);
    if (wifiConnected) {
        wifiRSSI = WiFi.RSSI();
        Serial.printf("[wifi] Connected — IP:%s RSSI:%d\n",
                      WiFi.localIP().toString().c_str(), wifiRSSI);
        lastStatus = "WiFi connected";
    } else {
        Serial.printf("[wifi] Connection FAILED (status=%d, ssid_len=%d, pass_len=%d)\n",
                      WiFi.status(), wifiSSID.length(), wifiPassword.length());
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
    // with the board's SPI pins before passing that bus to SD.begin().
    // Pin mapping is defined in hardware.h per board variant.
    SPI.begin(SD_PIN_CLK, SD_PIN_MISO, SD_PIN_MOSI, SD_PIN_CS);
    delay(10);

    // Try at 25 MHz first; drop to 4 MHz on failure (some cards need slower)
    if (!SD.begin(SD_PIN_CS, SPI, 25000000)) {
        Serial.println("[sd] 25 MHz init failed, retrying at 4 MHz...");
        SD.end();
        delay(50);
        if (!SD.begin(SD_PIN_CS, SPI, 4000000)) {
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
// Camera (CoreS3 only)
// =============================================================================
#if HAS_CAMERA

void initCamera() {
    camera_config_t config = {};
    config.ledc_channel = LEDC_CHANNEL_0;
    config.ledc_timer   = LEDC_TIMER_0;
    config.pin_d0       = 39;
    config.pin_d1       = 40;
    config.pin_d2       = 41;
    config.pin_d3       = 42;
    config.pin_d4       = 15;
    config.pin_d5       = 16;
    config.pin_d6       = 48;
    config.pin_d7       = 47;
    config.pin_xclk     = 2;
    config.pin_pclk     = 45;
    config.pin_vsync    = 46;
    config.pin_href     = 38;
    config.pin_sccb_sda = 12;
    config.pin_sccb_scl = 11;
    config.pin_pwdn     = -1;
    config.pin_reset    = -1;
    config.xclk_freq_hz = 20000000;
    config.pixel_format = PIXFORMAT_JPEG;
    config.frame_size   = FRAMESIZE_QVGA;   // 320x240 — matches display
    config.jpeg_quality = 12;                // decent quality, ~15-25 KB
    config.fb_count     = 1;
    config.fb_location  = CAMERA_FB_IN_PSRAM;
    config.grab_mode    = CAMERA_GRAB_LATEST;

    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK) {
        Serial.printf("[cam] init failed: 0x%x\n", err);
        cameraReady = false;
        return;
    }
    cameraReady = true;
    Serial.println("[cam] GC0308 ready (QVGA JPEG)");
}

// Capture a single JPEG frame into PSRAM.  Called once at recording start.
void captureSnapshot() {
    if (!cameraReady) return;
    if (g_snapBuf) { free(g_snapBuf); g_snapBuf = nullptr; g_snapLen = 0; }

    camera_fb_t* fb = esp_camera_fb_get();
    if (!fb) { Serial.println("[cam] snapshot failed"); return; }

    g_snapBuf = (uint8_t*)ps_malloc(fb->len);
    if (g_snapBuf) {
        memcpy(g_snapBuf, fb->buf, fb->len);
        g_snapLen = fb->len;
        Serial.printf("[cam] snapshot: %zu bytes JPEG\n", g_snapLen);
    } else {
        Serial.println("[cam] ps_malloc snapshot failed");
    }
    esp_camera_fb_return(fb);
}

#endif // HAS_CAMERA

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
        Serial.println("[audio] FATAL: No PSRAM. Device requires PSRAM for audio buffer.");
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

// =============================================================================
// SD-streaming helpers
// =============================================================================

// Decide whether to use SD-streaming for the current recording.
bool shouldUseSdStream() {
    return sdStreamForce || (recordDurationSec > CONT_MAX_DURATION_SEC);
}

// Open a new SD file for streaming audio.  Sets sdRecPath, sdRecUUID,
// resets counters.  Returns true on success.
bool openSdStreamFile() {
    if (!sdReady) {
        Serial.println("[sd-stream] SD not ready");
        return false;
    }
    ensureSDSpace();

    const char* ext = useMp3 ? "mp3" : "wav";
    const char* root = "/wav";
    char dirPath[64];
    buildDateDir(dirPath, sizeof(dirPath), root);
    mkdirP(dirPath);

    generateUUID(sdRecUUID);
    snprintf(sdRecPath, sizeof(sdRecPath), "%s/%.36s.%s", dirPath, sdRecUUID, ext);

    sdRecFile = SD.open(sdRecPath, FILE_WRITE);
    if (!sdRecFile) {
        Serial.printf("[sd-stream] open(%s) failed\n", sdRecPath);
        return false;
    }

    sdRecSamples = 0;
    sdRecBytes   = 0;

    if (!useMp3) {
        // Write placeholder WAV header — patched with final size on close
        uint8_t hdr[44];
        memset(hdr, 0, 44);
        sdRecFile.write(hdr, 44);
        sdRecBytes = 44;
    }

#if ENABLE_MP3
    if (useMp3) {
        mp3Enc = mp3_encoder_init(SAMPLE_RATE, MP3_BITRATE_KBPS);
        if (!mp3Enc) {
            Serial.println("[sd-stream] MP3 encoder init failed");
            sdRecFile.close();
            return false;
        }
        mp3SamplesPerFrame = shine_samples_per_pass(mp3Enc);
        mp3AccumCount = 0;
        Serial.printf("[sd-stream] MP3 encoder ready: %d samples/frame\n",
                      mp3SamplesPerFrame);
    }
#endif

    Serial.printf("[sd-stream] Recording to %s\n", sdRecPath);
    return true;
}

#if ENABLE_MP3
// Feed PCM samples into the MP3 frame accumulator.  Full frames are
// encoded and written to the open sdRecFile.
void feedMp3Accumulator(const int16_t* samples, size_t count) {
    size_t offset = 0;
    while (offset < count) {
        size_t space = (size_t)mp3SamplesPerFrame - mp3AccumCount;
        size_t take  = min(space, count - offset);
        memcpy(&mp3AccumBuf[mp3AccumCount], &samples[offset], take * 2);
        mp3AccumCount += take;
        offset += take;
        if (mp3AccumCount >= (uint16_t)mp3SamplesPerFrame) {
            int written = 0;
            const uint8_t* frame = mp3_encode_frame(mp3Enc, mp3AccumBuf, &written);
            if (frame && written > 0) {
                sdRecFile.write(frame, written);
                sdRecBytes += written;
            }
            mp3AccumCount = 0;
        }
    }
}
#endif

// Finish an SD-stream recording: flush MP3 / patch WAV header, close file.
void finishSdRecording() {
    M5.Mic.end();
    isRecording = false;

#if ENABLE_MP3
    if (useMp3 && mp3Enc) {
        // Zero-pad and encode the last partial frame
        if (mp3AccumCount > 0) {
            memset(&mp3AccumBuf[mp3AccumCount], 0,
                   ((size_t)mp3SamplesPerFrame - mp3AccumCount) * 2);
            int written = 0;
            const uint8_t* frame = mp3_encode_frame(mp3Enc, mp3AccumBuf, &written);
            if (frame && written > 0) {
                sdRecFile.write(frame, written);
                sdRecBytes += written;
            }
        }
        // Flush encoder
        int flushed = 0;
        const uint8_t* tail = mp3_encode_flush(mp3Enc, &flushed);
        if (tail && flushed > 0) {
            sdRecFile.write(tail, flushed);
            sdRecBytes += flushed;
        }
        shine_close(mp3Enc);
        mp3Enc = nullptr;
    }
#endif

    if (!useMp3) {
        // Patch the WAV header with the final sample count
        sdRecFile.seek(0);
        uint8_t hdr[44];
        writeWavHeader(hdr, sdRecSamples);
        sdRecFile.write(hdr, 44);
    }

    sdRecFile.close();
    sdSaved++;
    Serial.printf("[sd-stream] Finished: %s (%u samples, %u bytes)\n",
                  sdRecPath, sdRecSamples, sdRecBytes);
}

// =============================================================================
// Recording entry point
// =============================================================================

void startRecording() {
    // Decide: SD-streaming or legacy PSRAM path
    sdStreamMode = shouldUseSdStream();

    // Capture a camera thumbnail before mic DMA starts (CoreS3 only).
    // Done once per recording session; continuous-mode chunks reuse the first snap.
#if HAS_CAMERA
    if (!continuousMode || continuousChunks == 0) captureSnapshot();
#endif

    if (sdStreamMode) {
        // SD-streaming: no PSRAM audio buffers needed
        if (!openSdStreamFile()) {
            lastStatus = "SD stream failed!";
            return;
        }
    } else {
        // Legacy PSRAM path
        if (!audioBuffer && !allocateAudioBuffer()) return;

        int16_t* buf = continuousMode ? recordBuf : audioBufA;
        if (!buf) buf = audioBuffer;
        memset(buf, 0, audioPcmBytes());
        audioBuffer      = buf;
        audioBufferIndex = 0;
    }

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

    if (sdStreamMode) {
        lastStatus = (useMp3 ? "SD/MP3 " : "SD/WAV ") +
                     String(recordDurationSec) + " s...";
    } else {
        lastStatus = continuousMode
                     ? "Continuous: chunk 1..."
                     : "Recording " + String(recordDurationSec) + " s...";
    }

    memset(levelHistory, 0, sizeof(levelHistory));
    levelIdx = 0;

    Serial.printf("[audio] Recording started (sdStream=%d, mp3=%d, cont=%d). Target: %u s\n",
                  (int)sdStreamMode, (int)useMp3, (int)continuousMode,
                  recordDurationSec);
}

// Call from loop() while STATE_RECORDING or STATE_CONTINUOUS.
// Fills audio in 1024-sample chunks from the DMA ring buffer.
void recordAudioChunk() {
    if (!isRecording) return;

    // --- SD-streaming path ---
    if (sdStreamMode) {
        size_t remaining = audioSampleTarget() - sdRecSamples;
        if (remaining == 0) {
            if (continuousMode) return;  // swapAndContinue() handles it
            finishSdRecording();
            appState = STATE_ENCODING;
            Serial.printf("[sd-stream] Duration reached: %u samples\n", sdRecSamples);
            return;
        }

        size_t chunk = min((size_t)1024, remaining);
        static int16_t DRAM_ATTR sramChunk[1024];
        if (M5.Mic.record(sramChunk, chunk, SAMPLE_RATE)) {
            uint32_t t0 = millis();
            while (M5.Mic.isRecording() && (millis() - t0 < 500)) { delay(1); }

            // VU meter peak
            int peakVal = 0;
            for (size_t i = 0; i < chunk; i++) {
                int av = abs((int)sramChunk[i]);
                if (av > peakVal) peakVal = av;
            }
            levelHistory[levelIdx % LEVEL_BARS] = peakVal;
            levelIdx++;

            // Write to SD
#if ENABLE_MP3
            if (useMp3) {
                feedMp3Accumulator(sramChunk, chunk);
            } else
#endif
            {
                sdRecFile.write((const uint8_t*)sramChunk, chunk * 2);
                sdRecBytes += chunk * 2;
            }
            sdRecSamples += chunk;
        } else {
            delay(1);
        }

        // Check elapsed time
        if (millis() - recordStartMs >= (unsigned long)recordDurationSec * 1000UL) {
            if (!continuousMode) {
                finishSdRecording();
                appState = STATE_ENCODING;
                Serial.printf("[sd-stream] Duration reached: %u samples in %lu ms\n",
                              sdRecSamples, millis() - recordStartMs);
            }
        }
        return;
    }

    // --- Legacy PSRAM path (unchanged) ---
    size_t remaining = audioSampleTarget() - audioBufferIndex;
    if (remaining == 0) {
        if (continuousMode) {
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
// VConStream — streams vCon JSON from SD file with on-the-fly base64url.
// =============================================================================
// Used by the SD-streaming upload path.  Yields:
//   1. JSON prefix (metadata + "body":")
//   2. base64url-encoded audio file read from SD in B64_RAW_CHUNK_SIZE blocks
//   3. JSON suffix (closing brackets + attachments)
// HTTPClient calls read() to pull data; total Content-Length is pre-computed.

class VConStream : public Stream {
public:
    VConStream(const char* sdPath,
               const char* prefix, size_t prefixLen,
               const char* suffix, size_t suffixLen)
        : _prefix(prefix), _prefixLen(prefixLen),
          _suffix(suffix), _suffixLen(suffixLen),
          _phase(PHASE_PREFIX), _phasePos(0),
          _b64BufLen(0), _b64BufPos(0)
    {
        _sdFile = SD.open(sdPath, FILE_READ);
        _fileSize = _sdFile ? _sdFile.size() : 0;

        // Pre-compute base64url output length for the file
        size_t b64Std = ((_fileSize + 2) / 3) * 4;
        size_t padChars = (3 - (_fileSize % 3)) % 3;
        _b64UrlLen = b64Std - padChars;

        _totalLen = _prefixLen + _b64UrlLen + _suffixLen;
        _served = 0;
    }

    ~VConStream() { if (_sdFile) _sdFile.close(); }

    size_t totalLength() const { return _totalLen; }

    int available() override {
        return (int)(_totalLen - _served);
    }

    int read() override {
        uint8_t c;
        return (readBytes((char*)&c, 1) == 1) ? (int)c : -1;
    }

    size_t readBytes(char* buf, size_t len) override {
        size_t filled = 0;
        while (filled < len && _phase != PHASE_DONE) {
            switch (_phase) {
            case PHASE_PREFIX: {
                size_t rem = _prefixLen - _phasePos;
                size_t take = min(rem, len - filled);
                memcpy(buf + filled, _prefix + _phasePos, take);
                _phasePos += take;
                filled += take;
                _served += take;
                if (_phasePos >= _prefixLen) {
                    _phase = PHASE_BODY;
                    _phasePos = 0;
                }
                break;
            }
            case PHASE_BODY: {
                // Serve from the base64 encode buffer first
                if (_b64BufPos < _b64BufLen) {
                    size_t rem = _b64BufLen - _b64BufPos;
                    size_t take = min(rem, len - filled);
                    memcpy(buf + filled, _b64Buf + _b64BufPos, take);
                    _b64BufPos += take;
                    filled += take;
                    _served += take;
                    break;
                }
                // Refill: read raw bytes from SD, encode to base64url
                if (!_sdFile || !_sdFile.available()) {
                    _phase = PHASE_SUFFIX;
                    _phasePos = 0;
                    break;
                }
                uint8_t raw[B64_RAW_CHUNK_SIZE];
                size_t nRead = _sdFile.read(raw, B64_RAW_CHUNK_SIZE);
                if (nRead == 0) {
                    _phase = PHASE_SUFFIX;
                    _phasePos = 0;
                    break;
                }
                // Encode to standard base64
                size_t b64Len = 0;
                mbedtls_base64_encode(_b64Buf, sizeof(_b64Buf), &b64Len, raw, nRead);
                // Convert to base64url in-place, strip padding
                _b64BufLen = 0;
                for (size_t i = 0; i < b64Len; i++) {
                    uint8_t c = _b64Buf[i];
                    if (c == '=') continue;  // strip padding
                    if (c == '+') c = '-';
                    else if (c == '/') c = '_';
                    _b64Buf[_b64BufLen++] = c;
                }
                _b64BufPos = 0;
                // Don't break — loop back to serve from the freshly filled buffer
                break;
            }
            case PHASE_SUFFIX: {
                size_t rem = _suffixLen - _phasePos;
                size_t take = min(rem, len - filled);
                memcpy(buf + filled, _suffix + _phasePos, take);
                _phasePos += take;
                filled += take;
                _served += take;
                if (_phasePos >= _suffixLen) _phase = PHASE_DONE;
                break;
            }
            case PHASE_DONE:
                break;
            }
        }
        return filled;
    }

    // Stream requires these but we only need the read side
    size_t write(uint8_t) override { return 0; }
    int peek() override { return -1; }

    // Reset for retry — rewind SD file and restart from prefix
    void rewind() {
        if (_sdFile) _sdFile.seek(0);
        _phase = PHASE_PREFIX;
        _phasePos = 0;
        _b64BufLen = 0;
        _b64BufPos = 0;
        _served = 0;
    }

private:
    enum Phase { PHASE_PREFIX, PHASE_BODY, PHASE_SUFFIX, PHASE_DONE };

    const char* _prefix;
    size_t      _prefixLen;
    const char* _suffix;
    size_t      _suffixLen;
    File        _sdFile;
    size_t      _fileSize;
    size_t      _b64UrlLen;
    size_t      _totalLen;
    size_t      _served;
    Phase       _phase;
    size_t      _phasePos;

    // Base64 encode buffer: B64_RAW_CHUNK_SIZE raw → up to B64_ENC_CHUNK_SIZE+4 encoded
    uint8_t _b64Buf[B64_ENC_CHUNK_SIZE + 4];
    size_t  _b64BufLen;
    size_t  _b64BufPos;
};

// =============================================================================
// SD-streaming upload: build vCon JSON from SD file and POST via VConStream
// =============================================================================

bool buildAndUploadVConFromSD(const char* audioPath, const char* uuid,
                              uint32_t numSamples) {
    if (!wifiConnected) return false;

    Serial.printf("[vcon-sd] Building vCon from SD: %s (%u samples)\n",
                  audioPath, numSamples);

    // Gather metadata
    char timestamp[32];
    getTimestamp(timestamp, sizeof(timestamp));
    String macStr = WiFi.macAddress();

    const char* mime = useMp3 ? "audio/mpeg" : "audio/wav";

    char tagsRaw[320];
    snprintf(tagsRaw, sizeof(tagsRaw),
             "{\"source\":\"m5stack-core2\","
             "\"device_id\":\"%s\","
             "\"mac\":\"%s\","
             "\"sample_rate\":%d,"
             "\"duration_seconds\":%u,"
             "\"format\":\"%s\"}",
             deviceID.c_str(), macStr.c_str(), SAMPLE_RATE,
             (unsigned)(numSamples / SAMPLE_RATE), mime);
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
            "\"mimetype\":\"%s\","
            "\"encoding\":\"base64url\","
            "\"body\":\"",
        uuid, timestamp, macStr.c_str(), deviceID.c_str(), timestamp,
        (unsigned)(numSamples / SAMPLE_RATE), mime);

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

    // Build URL
    uploadTaskState = UTS_UPLOADING;
    String urlFinal = postURL;
    if (deviceToken.length() > 0) {
        urlFinal += (urlFinal.indexOf('?') >= 0) ? "&" : "?";
        urlFinal += "token=" + deviceToken;
    }

    Serial.printf("[vcon-sd] POST → %s\n", urlFinal.c_str());

    const unsigned long retryDelayMs[3] = { 5000UL, 30000UL, 300000UL };
    int httpCode = 0;

    for (int attempt = 0; attempt <= 3; attempt++) {
        if (attempt > 0) {
            uploadTaskState = UTS_RETRY;
            uploadRetryNum  = (uint8_t)attempt;
            unsigned long waitMs = retryDelayMs[attempt - 1];
            Serial.printf("[vcon-sd] Retry %d in %lu ms\n", attempt, waitMs);
            unsigned long t0 = millis();
            while (millis() - t0 < waitMs) { yield(); delay(10); }
            uploadTaskState = UTS_UPLOADING;
        }

        VConStream vcs(audioPath, prefix, (size_t)prefixLen,
                       suffix, (size_t)suffixLen);
        size_t totalLen = vcs.totalLength();

        Serial.printf("[vcon-sd] Stream total: %zu bytes\n", totalLen);

        HTTPClient http;
        http.begin(urlFinal);
        http.addHeader("Content-Type", "application/json");
        if (deviceToken.length() > 0)
            http.addHeader("X-Device-Token", deviceToken);
        http.setTimeout(120000);  // longer timeout for large files

        httpCode = http.sendRequest("POST", &vcs, totalLen);
        if (httpCode > 0) {
            String resp = http.getString();
            if (resp.length() > 80) resp = resp.substring(0, 77) + "...";
            Serial.printf("[vcon-sd] HTTP %d: %s\n", httpCode, resp.c_str());
        } else {
            Serial.printf("[vcon-sd] Network error: %d\n", httpCode);
        }
        http.end();

        if (httpCode == 202 || httpCode == 200) break;
        if (httpCode == 400) break;
    }

    uploadTaskHttpCode = (int32_t)httpCode;

    if (httpCode == 202 || httpCode == 200) {
        totalUploads++;
        saveConfig();
        uploadTaskState = UTS_DONE_OK;
        Serial.printf("[vcon-sd] SUCCESS HTTP %d  UUID:%s\n", httpCode, uuid);
        return true;
    } else {
        failedUploads++;
        saveConfig();
        uploadTaskState = UTS_DONE_FAIL;
        Serial.printf("[vcon-sd] FAILED HTTP %d\n", httpCode);
        return false;
    }
}

// =============================================================================
// Build vCon JSON and POST (legacy PSRAM path)
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
             "{\"source\":\"" DEVICE_TYPE_STR "\","
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
              "\"device_type\":\"" DEVICE_TYPE_STR "\""
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

    // ---- Build suffix (closes dialog body, adds attachments) ---------------
    // The suffix is heap-allocated because the optional camera thumbnail
    // attachment can add ~30 KB of base64url data (CoreS3 only).
    char* snapB64 = nullptr;
    size_t snapB64Len = 0;

#if HAS_CAMERA
    if (g_snapBuf && g_snapLen > 0) {
        size_t maxB64 = ((g_snapLen + 2) / 3) * 4 + 1;
        uint8_t* b64Tmp = (uint8_t*)ps_malloc(maxB64);
        if (b64Tmp) {
            size_t rawLen = 0;
            mbedtls_base64_encode(b64Tmp, maxB64, &rawLen, g_snapBuf, g_snapLen);
            // Convert standard base64 → base64url in-place
            for (size_t i = 0; i < rawLen; i++) {
                uint8_t c = b64Tmp[i];
                if (c == '=') break;
                if (c == '+') c = '-';
                else if (c == '/') c = '_';
                b64Tmp[snapB64Len++] = c;
            }
            b64Tmp[snapB64Len] = '\0';
            snapB64 = (char*)b64Tmp;
            Serial.printf("[cam] thumbnail base64url: %zu bytes\n", snapB64Len);
        }
        free(g_snapBuf); g_snapBuf = nullptr; g_snapLen = 0;
    }
#endif

    size_t suffixBufSize = 768 + snapB64Len;
    char* suffix = (char*)ps_malloc(suffixBufSize);
    if (!suffix) {
        if (snapB64) free(snapB64);
        Serial.println("[vcon] ps_malloc suffix failed");
        uploadTaskState = UTS_DONE_FAIL;
        return false;
    }

    int suffixLen;
    if (snapB64) {
        suffixLen = snprintf(suffix, suffixBufSize,
            "\"}],"
            "\"analysis\":[],"
            "\"attachments\":[{"
              "\"purpose\":\"tags\","
              "\"start\":\"%s\","
              "\"party\":0,"
              "\"dialog\":0,"
              "\"encoding\":\"json\","
              "\"body\":\"%s\""
            "},{"
              "\"purpose\":\"thumbnail\","
              "\"start\":\"%s\","
              "\"party\":0,"
              "\"dialog\":0,"
              "\"mimetype\":\"image/jpeg\","
              "\"encoding\":\"base64url\","
              "\"body\":\"%s\""
            "}]}",
            timestamp, tagsEsc,
            timestamp, snapB64);
        free(snapB64); snapB64 = nullptr;
    } else {
        suffixLen = snprintf(suffix, suffixBufSize,
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
    }

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
        free(suffix);
        Serial.printf("[vcon] ps_malloc json (%zu) failed. FreePS:%u\n",
                      jsonBufSize, ESP.getFreePsram());
        uploadTaskState = UTS_DONE_FAIL;
        return false;
    }
    memcpy(jsonBuf, prefix, prefixLen);

    uint8_t* wavBuf = (uint8_t*)ps_malloc(wavBytes);
    if (!wavBuf) {
        free(suffix);
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
        free(suffix);
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
    free(suffix);   // suffix is now in jsonBuf
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
    if (!wifiConnected) { lastStatus = "No WiFi connection"; return false; }

    // SD-streaming path: audio is already on SD — use VConStream upload
    if (sdStreamMode) {
        if (sdRecSamples == 0) { lastStatus = "No audio recorded"; return false; }

        appState   = STATE_ENCODING;
        lastStatus = useMp3 ? "Uploading MP3..." : "Uploading WAV...";
        updateDisplay();

        uploadTaskState    = UTS_IDLE;
        uploadTaskHttpCode = 0;

        bool ok = buildAndUploadVConFromSD(sdRecPath, sdRecUUID, sdRecSamples);

        lastHttpCode = (int)uploadTaskHttpCode;
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

    // Legacy PSRAM path
    if (audioBufferIndex == 0) { lastStatus = "No audio recorded"; return false; }

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

// SD-streaming upload task — uploads a completed SD file on core 0.
struct SdUploadJob {
    char path[120];
    char uuid[37];
    uint32_t numSamples;
};
static SdUploadJob g_sdUploadJob;

void sdUploadTaskFn(void* /*param*/) {
    buildAndUploadVConFromSD(g_sdUploadJob.path, g_sdUploadJob.uuid,
                             g_sdUploadJob.numSamples);
    continuousChunks++;
    uploadBusy = false;
    vTaskDelete(nullptr);
}

void startSdUploadTask(const char* path, const char* uuid, uint32_t numSamples) {
    strncpy(g_sdUploadJob.path, path, sizeof(g_sdUploadJob.path) - 1);
    strncpy(g_sdUploadJob.uuid, uuid, sizeof(g_sdUploadJob.uuid) - 1);
    g_sdUploadJob.numSamples = numSamples;
    uploadBusy      = true;
    uploadTaskState = UTS_ENCODING;
    xTaskCreatePinnedToCore(
        sdUploadTaskFn, "vcon_sd_up",
        16 * 1024, nullptr, 1,
        &g_uploadTaskHandle, 0);
}

// Called every loop() tick in continuous mode.
void swapAndContinue() {
    // --- SD-streaming continuous swap ---
    if (sdStreamMode) {
        bool durationDone = (millis() - recordStartMs >=
                             (unsigned long)recordDurationSec * 1000UL);
        bool samplesDone  = (sdRecSamples >= audioSampleTarget());

        if (!(durationDone || samplesDone)) return;

        if (uploadBusy) {
            if (samplesDone) {
                // Spinwait — DMA ring buffer absorbs audio while we wait
                Serial.println("[cont-sd] upload busy — waiting...");
                while (uploadBusy) { yield(); delay(5); }
            } else {
                return;
            }
        }

        // Finish the current SD file
        finishSdRecording();

        // Save completed file info for the upload task
        char donePath[120];
        char doneUUID[37];
        strncpy(donePath, sdRecPath, sizeof(donePath));
        strncpy(doneUUID, sdRecUUID, sizeof(doneUUID));
        uint32_t doneSamples = sdRecSamples;

        if (stopContinuous) {
            M5.Mic.end();
            isRecording    = false;
            continuousMode = false;
            stopContinuous = false;
            sdStreamMode   = false;
            appState       = STATE_UPLOADING;
            lastStatus     = "Finishing last chunk...";
        } else {
            // Open a new SD file and resume recording immediately
            if (!openSdStreamFile()) {
                M5.Mic.end();
                isRecording    = false;
                continuousMode = false;
                sdStreamMode   = false;
                appState       = STATE_ERROR;
                lastStatus     = "SD stream failed!";
                return;
            }
            isRecording   = true;
            recordStartMs = millis();
            lastStatus = "SD continuous: chunk " +
                         String((uint32_t)continuousChunks + 1) + "...";
        }

        Serial.printf("[cont-sd] swap → uploading %u samples from %s\n",
                      doneSamples, donePath);
        startSdUploadTask(donePath, doneUUID, doneSamples);
        return;
    }

    // --- Legacy PSRAM continuous swap ---
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
        gfx->drawFastVLine(x + i, y, h, col);
    }
}

// Panel: clear interior, draw border + label bar
void drawPanel(int x, int y, int w, int h, const char* label,
               uint16_t labelBg   = VC_GREEN,
               uint16_t labelFg   = TFT_BLACK,
               uint16_t borderCol = VC_GREEN) {
    gfx->fillRect(x + 1, y + 1, w - 2, h - 2, TFT_BLACK);
    gfx->drawRect(x, y, w, h, borderCol);
    gfx->fillRect(x + 1, y + 1, w - 2, 11, labelBg);
    gfx->setTextSize(1);
    gfx->setTextColor(labelFg, labelBg);
    gfx->setCursor(x + 3, y + 3);
    gfx->print(label);
}

// 16-bar VU meter inside the AUDIO panel
void drawLevelBars(int x, int y, int w, int h) {
    int barW = (w - LEVEL_BARS) / LEVEL_BARS;   // e.g. (158-16)/16 = 8 px
    int gap  = 1;

    // One fillRect clears the entire bars region — gaps, right-side remainder,
    // and any stale pixels from previous screens.  Intermediate states are
    // invisible because all drawing targets the off-screen frameCanvas;
    // updateDisplay() pushes the completed frame in a single transfer.
    gfx->fillRect(x, y, w, h, TFT_BLACK);

    if (appState != STATE_RECORDING) {
        // Show a dim flat-line so the panel doesn't look dead when idle
        gfx->drawFastHLine(x, y + h / 2, w, 0x2945u);
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
            gfx->fillRect(bx, y + h - barH, barW, barH, col);
        }
    }
}

// Progress bar (filled rect + outline)
void drawProgressBar(int x, int y, int w, int h, float pct, uint16_t fillCol = TFT_CYAN) {
    gfx->drawRect(x, y, w, h, TFT_WHITE);
    int filled = (int)(pct * (float)(w - 2));
    if (filled > 0)
        gfx->fillRect(x + 1, y + 1, filled, h - 2, fillCol);
    gfx->fillRect(x + 1 + filled, y + 1, w - 2 - filled, h - 2, TFT_BLACK);
}

// =============================================================================
// UI: Button row helper
// =============================================================================

void drawButtonRow(const char* labelA, const char* labelB, const char* labelC,
                   uint16_t colA = VC_GREEN,
                   uint16_t colB = VC_GREEN,
                   uint16_t colC = VC_GREEN) {
    gfx->fillRect(0, UI_BTN_Y, SCREEN_W, UI_BTN_H, TFT_BLACK);
    gfx->drawFastHLine(0, UI_BTN_Y, SCREEN_W, VC_GREEN);
    const int btnW = SCREEN_W / 3;
    const char* labels[3] = { labelA, labelB, labelC };
    uint16_t    colors[3] = { colA,   colB,   colC   };
    for (int i = 0; i < 3; i++) {
        int bx = i * btnW;
        if (i < 2) gfx->drawFastVLine(bx + btnW, UI_BTN_Y + 1, UI_BTN_H - 1, VC_GREEN);
        gfx->drawRect(bx + 4, UI_BTN_Y + 4, btnW - 8, UI_BTN_H - 8, colors[i]);
        gfx->setTextSize(2);
        int lw = (int)strlen(labels[i]) * 12;
        gfx->setTextColor(colors[i], TFT_BLACK);
        gfx->setCursor(bx + (btnW - lw) / 2, UI_BTN_Y + 14);
        gfx->print(labels[i]);
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

    gfx->fillRect(0, UI_CONTENT_Y, SCREEN_W, 120, stateBg);

    // Device ID — small, top-left
    gfx->setTextSize(1);
    gfx->setTextColor(stateFg == TFT_WHITE ? 0xBDF7u : 0x4208u, stateBg);
    gfx->setCursor(6, UI_CONTENT_Y + 4);
    gfx->print(deviceID.c_str());

    // WiFi dot — top-right
    gfx->setTextColor(wifiConnected ? TFT_GREEN : TFT_RED, stateBg);
    gfx->setCursor(SCREEN_W - 54, UI_CONTENT_Y + 4);
    gfx->print(wifiConnected ? "\x07 WiFi" : "\x18 WiFi");

    // Big state label — vertically centred in band
    gfx->setTextSize(4);
    gfx->setTextColor(stateFg, stateBg);
    int sw = (int)strlen(stateStr) * 24;
    gfx->setCursor((SCREEN_W - sw) / 2, UI_CONTENT_Y + 36);
    gfx->print(stateStr);

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
        gfx->setTextSize(1);
        gfx->setTextColor(stateFg, stateBg);
        int tw = (int)strlen(timerbuf) * 6;
        gfx->setCursor((SCREEN_W - tw) / 2, UI_CONTENT_Y + 103);
        gfx->print(timerbuf);
        if (appState == STATE_CONTINUOUS) {
            gfx->setTextColor(VC_GREEN, stateBg);
            gfx->setCursor(SCREEN_W - 70, UI_CONTENT_Y + 103);
            gfx->printf("Chunk#%u", (uint32_t)continuousChunks + 1);
        }
    }

    // ── STATS BAND (y=128..163) ────────────────────────────────────────────
    gfx->fillRect(0, 128, SCREEN_W, 35, TFT_BLACK);
    gfx->drawFastHLine(0, 128, SCREEN_W, VC_GREEN);
    gfx->drawFastHLine(0, 163, SCREEN_W, 0x2945u);

    gfx->setTextSize(2);
    gfx->setTextColor(TFT_GREEN, TFT_BLACK);
    gfx->setCursor(8, 135);
    gfx->printf("OK:%-5u", totalUploads);
    gfx->setTextColor(TFT_RED, TFT_BLACK);
    gfx->setCursor(152, 135);
    gfx->printf("Err:%u", failedUploads);

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
            gfx->setTextSize(1);
            gfx->setTextColor(utsCol, TFT_BLACK);
            gfx->setCursor(SCREEN_W - 40, 135);
            gfx->print(utsStr);
        }
    }

    // ── STATUS ROW (y=163..193) ────────────────────────────────────────────
    gfx->fillRect(0, 163, SCREEN_W, 31, TFT_BLACK);
    gfx->setTextSize(1);
    uint16_t statusCol = TFT_WHITE;
    if (appState == STATE_SUCCESS) statusCol = TFT_GREEN;
    if (appState == STATE_ERROR)   statusCol = TFT_RED;
    gfx->setTextColor(statusCol, TFT_BLACK);
    gfx->setCursor(6, 168);
    // Truncate to fit one line
    String st = lastStatus;
    if (st.length() > 52) st = st.substring(0, 49) + "...";
    gfx->print(st.c_str());

    // Last HTTP code + dur hint
    gfx->setTextColor(TFT_DARKGREY, TFT_BLACK);
    gfx->setCursor(6, 181);
    gfx->printf("dur:%us", recordDurationSec);
    if (lastHttpCode > 0) {
        gfx->setTextColor(lastHttpCode == 202 ? TFT_GREEN :
                                lastHttpCode == 200 ? TFT_YELLOW : TFT_RED,
                                TFT_BLACK);
        gfx->printf("  HTTP %d", lastHttpCode);
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
    gfx->fillRect(0, UI_CONTENT_Y, SCREEN_W, 14, VC_GREEN);
    gfx->setTextSize(1);
    gfx->setTextColor(TFT_BLACK, VC_GREEN);
    gfx->setCursor(4, UI_CONTENT_Y + 3);
    gfx->print("STATUS \x7e RECORDING DASHBOARD");

    // ── VU meter (y=22..62, 40px tall) ────────────────────────────────────
    const int vx = 0, vy = 22, vw = SCREEN_W, vh = 40;
    gfx->fillRect(vx, vy, vw, vh, TFT_BLACK);
    gfx->drawRect(vx, vy, vw, vh, activeRec ? TFT_RED : VC_GREEN);
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
            gfx->fillRect(bx, vy + vh - 1 - barH, barW, barH, col);
        }
    } else {
        gfx->drawFastHLine(vx+1, vy + vh/2, vw-2, 0x2945u);
        gfx->setTextSize(1);
        gfx->setTextColor(TFT_DARKGREY, TFT_BLACK);
        gfx->setCursor(vx + vw/2 - 30, vy + vh/2 - 4);
        gfx->print("not recording");
    }

    // ── Elapsed / progress (y=63..76) ─────────────────────────────────────
    gfx->fillRect(0, 63, SCREEN_W, 14, TFT_BLACK);
    gfx->drawFastHLine(0, 63, SCREEN_W, 0x2945u);
    if (activeRec || appState == STATE_ENCODING || appState == STATE_UPLOADING) {
        unsigned long elapsed = activeRec ? (millis() - recordStartMs) / 1000 : 0;
        float prog = activeRec ? recordingProgress() : 1.0f;
        uint16_t barCol = (appState == STATE_CONTINUOUS) ? VC_GREEN :
                          (appState == STATE_RECORDING)  ? TFT_RED  : TFT_CYAN;
        drawProgressBar(4, 64, SCREEN_W - 60, 8, prog, barCol);
        gfx->setTextSize(1);
        gfx->setTextColor(TFT_YELLOW, TFT_BLACK);
        gfx->setCursor(SCREEN_W - 54, 65);
        gfx->printf("%02lu:%02lu", elapsed / 60, elapsed % 60);
    } else {
        gfx->setTextSize(1);
        gfx->setTextColor(TFT_DARKGREY, TFT_BLACK);
        gfx->setCursor(4, 65);
        gfx->print("Idle \x7e press HOME \x7e RUN");
    }

    // ── Upload state (y=77..90) ────────────────────────────────────────────
    gfx->fillRect(0, 77, SCREEN_W, 14, TFT_BLACK);
    gfx->drawFastHLine(0, 77, SCREEN_W, 0x2945u);
    gfx->setTextSize(1);
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
    gfx->setTextColor(utsCol, TFT_BLACK);
    gfx->setCursor(4, 80);
    gfx->printf("Upload: %-8s", utsStr);
    uint16_t httpCol = (lastHttpCode >= 200 && lastHttpCode < 300) ? TFT_GREEN :
                       (lastHttpCode == 0) ? TFT_DARKGREY : TFT_RED;
    gfx->setTextColor(httpCol, TFT_BLACK);
    gfx->setCursor(160, 80);
    if (lastHttpCode > 0) gfx->printf("HTTP %d", lastHttpCode);
    else                   gfx->print("HTTP --");

    // ── Counters: OK / Err / Chunk (y=91..104) ────────────────────────────
    gfx->fillRect(0, 91, SCREEN_W, 14, TFT_BLACK);
    gfx->drawFastHLine(0, 91, SCREEN_W, 0x2945u);
    gfx->setTextSize(1);
    gfx->setTextColor(TFT_GREEN,  TFT_BLACK);
    gfx->setCursor(4,   94); gfx->printf("OK:%u", totalUploads);
    gfx->setTextColor(TFT_RED,    TFT_BLACK);
    gfx->setCursor(80,  94); gfx->printf("Err:%u", failedUploads);
    gfx->setTextColor(VC_GREEN,   TFT_BLACK);
    gfx->setCursor(165, 94); gfx->printf("Chunk #%u", (uint32_t)continuousChunks);

    // ── WiFi (y=105..118) ─────────────────────────────────────────────────
    gfx->fillRect(0, 105, SCREEN_W, 14, TFT_BLACK);
    gfx->drawFastHLine(0, 105, SCREEN_W, 0x2945u);
    gfx->setTextSize(1);
    if (wifiConnected) {
        gfx->setTextColor(TFT_GREEN,  TFT_BLACK);
        gfx->setCursor(4, 108); gfx->print("WiFi \x07 ");
        gfx->setTextColor(TFT_WHITE,  TFT_BLACK);
        String s = wifiSSID; if (s.length() > 14) s = s.substring(0, 13) + "~";
        gfx->print(s.c_str());
        gfx->setTextColor(TFT_CYAN, TFT_BLACK);
        gfx->setCursor(160, 108);
        gfx->print(WiFi.localIP().toString().c_str());
        gfx->setTextColor(TFT_DARKGREY, TFT_BLACK);
        gfx->setCursor(270, 108);
        gfx->printf("%d", wifiRSSI);
    } else {
        gfx->setTextColor(TFT_RED, TFT_BLACK);
        gfx->setCursor(4, 108); gfx->print("WiFi \x18 NOT CONNECTED");
    }

    // ── Time / Date (y=119..132) ───────────────────────────────────────────
    gfx->fillRect(0, 119, SCREEN_W, 14, TFT_BLACK);
    gfx->drawFastHLine(0, 119, SCREEN_W, 0x2945u);
    gfx->setTextSize(1);
    gfx->setTextColor(VC_GREEN, TFT_BLACK);
    char tbuf[12], dbuf[12];
    getTimeDisplay(tbuf, sizeof(tbuf));
    getDateDisplay(dbuf, sizeof(dbuf));
    gfx->setCursor(4, 122);
    gfx->printf("%s UTC", tbuf);
    gfx->setTextColor(TFT_WHITE, TFT_BLACK);
    gfx->setCursor(160, 122);
    gfx->print(dbuf);

    // ── Uptime / PSRAM (y=133..146) ───────────────────────────────────────
    gfx->fillRect(0, 133, SCREEN_W, 14, TFT_BLACK);
    gfx->drawFastHLine(0, 133, SCREEN_W, 0x2945u);
    gfx->setTextSize(1);
    gfx->setTextColor(TFT_WHITE, TFT_BLACK);
    unsigned long sec = millis() / 1000;
    gfx->setCursor(4, 136);
    gfx->printf("Up %02lu:%02lu:%02lu", (sec/3600)%24, (sec/60)%60, sec%60);
    gfx->setTextColor(TFT_DARKGREY, TFT_BLACK);
    gfx->setCursor(160, 136);
    gfx->printf("PSRAM %uKB", ESP.getFreePsram() / 1024);

    // ── Mode / dur / SD (y=147..160) ──────────────────────────────────────
    gfx->fillRect(0, 147, SCREEN_W, 14, TFT_BLACK);
    gfx->drawFastHLine(0, 147, SCREEN_W, 0x2945u);
    gfx->setTextSize(1);
    bool contOK = (recordDurationSec <= CONT_MAX_DURATION_SEC);
    gfx->setTextColor(contOK ? TFT_GREEN : TFT_YELLOW, TFT_BLACK);
    gfx->setCursor(4, 150);
    gfx->printf("%s  %us", contOK ? "CONT" : "SINGLE", recordDurationSec);
    gfx->setTextColor(sdReady ? TFT_GREEN : TFT_RED, TFT_BLACK);
    gfx->setCursor(160, 150);
    gfx->printf("SD saved:%u", sdSaved);

    // ── Button row ─────────────────────────────────────────────────────────
    drawButtonRow("HOME", "--", "--",
                  VC_GREEN, TFT_DARKGREY, TFT_DARKGREY);
}

// =============================================================================
// UI: Config screen
// =============================================================================

void drawConfigScreen() {
    // Header
    gfx->fillRect(0, UI_CONTENT_Y, SCREEN_W, 14, VC_GREEN);
    gfx->setTextSize(1);
    gfx->setTextColor(TFT_BLACK, VC_GREEN);
    gfx->setCursor(4, UI_CONTENT_Y + 3);
    gfx->print("CONFIGURATION");

    int y = UI_CONTENT_Y + 16;
    const int lh = 14;
    gfx->fillRect(0, y, SCREEN_W, UI_CONTENT_H - 16, TFT_BLACK);

    auto kv = [&](const char* key, const char* val, uint16_t vc = TFT_WHITE) {
        gfx->setTextSize(1);
        gfx->setTextColor(VC_GREEN, TFT_BLACK);
        gfx->setCursor(4, y);
        gfx->print(key);
        gfx->setTextColor(vc, TFT_BLACK);
        gfx->setCursor(120, y);
        gfx->print(val);
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
        gfx->setTextColor(TFT_YELLOW, TFT_BLACK);
        gfx->setCursor(4, y); gfx->print("POST URL:"); y += lh;
        gfx->setTextColor(TFT_MAGENTA, TFT_BLACK);
        String u = postURL;
        while (u.length() > 0 && y < UI_CONTENT_Y + UI_CONTENT_H - 2) {
            int take = min((int)u.length(), 44);
            gfx->setCursor(4, y);
            gfx->print(u.substring(0, take));
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
    gfx->fillRect(0, UI_CONTENT_Y, SCREEN_W, 14, VC_GREEN);
    gfx->setTextSize(1);
    gfx->setTextColor(TFT_BLACK, VC_GREEN);
    gfx->setCursor(4, UI_CONTENT_Y + 3);
    gfx->print("RECORDING DURATION");
    gfx->fillRect(0, UI_CONTENT_Y + 14, SCREEN_W, UI_CONTENT_H - 14, TFT_BLACK);

    bool cont = (durEditPending <= CONT_MAX_DURATION_SEC);
    bool sdMode = (durEditPending > CONT_MAX_DURATION_SEC) || sdStreamForce;

    // ── Large centred value ──────────────────────────────────────────────────
    char nbuf[12];
    if (durEditPending >= 120) {
        // Show as MM:SS
        snprintf(nbuf, sizeof(nbuf), "%u:%02u",
                 durEditPending / 60, durEditPending % 60);
    } else {
        snprintf(nbuf, sizeof(nbuf), "%u", durEditPending);
    }
    int numGlyphs = strlen(nbuf);
    int numW = numGlyphs * 30 + 30;
    int numX = (SCREEN_W - numW) / 2;
    if (numX < 4) numX = 4;
    gfx->setTextSize(5);
    uint16_t valColor = sdMode ? TFT_CYAN : (cont ? TFT_GREEN : TFT_YELLOW);
    gfx->setTextColor(valColor, TFT_BLACK);
    gfx->setCursor(numX, 28);
    gfx->print(nbuf);
    if (durEditPending < 120) {
        gfx->setTextSize(2);
        gfx->setTextColor(TFT_WHITE, TFT_BLACK);
        gfx->print(" s");
    }

    // ── Range bar (y=90) ────────────────────────────────────────────────────
    const int bx = 10, by = 92, bw = SCREEN_W - 20, bh = 12;
    float frac = (float)(durEditPending - MIN_RECORD_DURATION_SEC) /
                 (float)(MAX_RECORD_DURATION_SEC - MIN_RECORD_DURATION_SEC);
    drawProgressBar(bx, by, bw, bh, frac, cont ? TFT_GREEN : TFT_YELLOW);

    // Tick line at CONT_MAX_DURATION_SEC
    int contMaxX = bx + (int)((float)(CONT_MAX_DURATION_SEC - MIN_RECORD_DURATION_SEC) /
                               (float)(MAX_RECORD_DURATION_SEC - MIN_RECORD_DURATION_SEC) * bw);
    gfx->drawFastVLine(contMaxX, by - 5, bh + 10, TFT_CYAN);

    // Min / CONT_MAX / Max labels
    gfx->setTextSize(1);
    gfx->setTextColor(TFT_DARKGREY, TFT_BLACK);
    gfx->setCursor(bx, by + bh + 4);
    gfx->printf("%us", MIN_RECORD_DURATION_SEC);
    gfx->setTextColor(TFT_CYAN, TFT_BLACK);
    char cmLabel[8]; snprintf(cmLabel, sizeof(cmLabel), "%us", CONT_MAX_DURATION_SEC);
    int cmLabelX = contMaxX - (int)(strlen(cmLabel) * 6) / 2;
    gfx->setCursor(cmLabelX, by - 14);
    gfx->print(cmLabel);
    gfx->setTextColor(TFT_DARKGREY, TFT_BLACK);
    char maxLabel[12];
    if (MAX_RECORD_DURATION_SEC >= 120)
        snprintf(maxLabel, sizeof(maxLabel), "%u:%02u",
                 MAX_RECORD_DURATION_SEC / 60, MAX_RECORD_DURATION_SEC % 60);
    else
        snprintf(maxLabel, sizeof(maxLabel), "%us", MAX_RECORD_DURATION_SEC);
    gfx->setCursor(bx + bw - (int)strlen(maxLabel) * 6, by + bh + 4);
    gfx->print(maxLabel);

    // ── Mode badge ──────────────────────────────────────────────────────────
    gfx->setTextSize(1);
    if (sdMode) {
        gfx->setTextColor(TFT_CYAN, TFT_BLACK);
        gfx->setCursor(10, 134);
        gfx->printf("\x7e SD-STREAM  (%s, up to 60 min)",
                     useMp3 ? "MP3" : "WAV");
    } else if (cont) {
        gfx->setTextColor(TFT_GREEN, TFT_BLACK);
        gfx->setCursor(10, 134);
        gfx->print("\x7e CONTINUOUS  (zero-gap dual-buffer)");
    } else {
        gfx->setTextColor(TFT_YELLOW, TFT_BLACK);
        gfx->setCursor(10, 134);
        gfx->print("\x7e SINGLE-SHOT  (>45s, uploads between chunks)");
    }

    // ── Hint line ───────────────────────────────────────────────────────────
    gfx->setTextColor(TFT_DARKGREY, TFT_BLACK);
    gfx->setCursor(10, 152);
    uint32_t stepSize = (durEditPending >= 120) ? 60 : 5;
    gfx->printf("Steps: %u s  |  SAVE stores to flash", stepSize);

    // Unsaved-change indicator
    if (durEditPending != recordDurationSec) {
        gfx->setTextColor(TFT_YELLOW, TFT_BLACK);
        gfx->setCursor(10, 166);
        gfx->printf("Current: %us  \x7e  pending: %us",
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
    gfx->fillRect(0, UI_CONTENT_Y, SCREEN_W, 14, VC_GREEN);
    gfx->setTextSize(1);
    gfx->setTextColor(TFT_BLACK, VC_GREEN);
    gfx->setCursor(4, UI_CONTENT_Y + 3);
    gfx->print("TOOLS \x7e DIAGNOSTICS");

    gfx->fillRect(0, UI_CONTENT_Y + 14, SCREEN_W, UI_CONTENT_H - 14, TFT_BLACK);

    if (toolsPhase == TPHASE_SELECT) {
        // Menu items (y=22..186, 5 items × 32px each)
        for (int i = 0; i < TOOL_COUNT; i++) {
            int iy = UI_CONTENT_Y + 14 + i * 33;
            bool sel = (i == (int)toolsSelectedItem);
            uint16_t bg = sel ? 0x1082u : TFT_BLACK;   // dark highlight
            gfx->fillRect(0, iy, SCREEN_W, 32, bg);
            if (sel) {
                gfx->fillRect(0, iy, 4, 32, VC_GREEN);  // left accent bar
                gfx->drawRect(0, iy, SCREEN_W, 32, VC_GREEN);
            }
            gfx->setTextSize(2);
            gfx->setTextColor(sel ? VC_GREEN : TFT_WHITE, bg);
            gfx->setCursor(12, iy + 8);
            gfx->print(itemLabels[i]);
            // Danger label for restart
            if (i == TOOL_RESTART) {
                gfx->setTextSize(1);
                gfx->setTextColor(TFT_RED, bg);
                gfx->setCursor(200, iy + 12);
                gfx->print("REBOOT");
            }
        }
        drawButtonRow("PREV", "RUN", "HOME",
                      VC_GREEN, TFT_GREEN, VC_GREEN);

    } else if (toolsPhase == TPHASE_CONFIRM) {
        gfx->setTextSize(2);
        gfx->setTextColor(TFT_RED, TFT_BLACK);
        gfx->setCursor(60, 80);
        gfx->print("Restart device?");
        gfx->setTextSize(1);
        gfx->setTextColor(TFT_WHITE, TFT_BLACK);
        gfx->setCursor(40, 110);
        gfx->print("This will reboot the M5Stack.");
        gfx->setCursor(40, 124);
        gfx->print("Any active recording will stop.");
        drawButtonRow("--", "CONFIRM", "CANCEL",
                      TFT_DARKGREY, TFT_RED, VC_GREEN);

    } else if (toolsPhase == TPHASE_RUNNING) {
        const char* itemLabels2[TOOL_COUNT] = {
            "Testing WiFi...", "Testing OTA...", "Testing POST...",
            "Reading SD...", "Restarting..."
        };
        gfx->setTextSize(2);
        gfx->setTextColor(TFT_CYAN, TFT_BLACK);
        gfx->setCursor(20, 80);
        gfx->print(itemLabels2[toolsSelectedItem]);
        // Animated dots
        int dots = (millis() / 400) % 4;
        gfx->setTextColor(TFT_YELLOW, TFT_BLACK);
        gfx->setCursor(20, 110);
        for (int d = 0; d < dots; d++) gfx->print(". ");
        drawButtonRow("--", "--", "--",
                      TFT_DARKGREY, TFT_DARKGREY, TFT_DARKGREY);

    } else {  // TPHASE_RESULT
        gfx->setTextSize(2);
        uint16_t resCol = toolsResultOK ? TFT_GREEN : TFT_RED;
        gfx->setTextColor(resCol, TFT_BLACK);
        gfx->setCursor(20, 40);
        gfx->print(toolsResultOK ? "PASS" : "FAIL");
        gfx->setTextSize(1);
        gfx->setTextColor(TFT_WHITE, TFT_BLACK);
        gfx->setCursor(20, 70); gfx->print(toolsResultLine1.c_str());
        gfx->setCursor(20, 86); gfx->print(toolsResultLine2.c_str());
        // Time remaining auto-clear bar
        unsigned long elapsed = millis() - toolsResultMs;
        float remaining = 1.0f - min(1.0f, (float)elapsed / (float)TOOLS_RESULT_TIMEOUT_MS);
        drawProgressBar(20, 110, SCREEN_W - 40, 6, remaining, TFT_DARKGREY);
        gfx->setTextSize(1);
        gfx->setTextColor(TFT_DARKGREY, TFT_BLACK);
        gfx->setCursor(20, 122);
        gfx->print("Auto-returns to menu...");
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
        gfx->fillRect(bx, y + (10 - bh), 4, bh, col);
    }
}

void drawWifiScreen() {
    // Header
    gfx->fillRect(0, UI_CONTENT_Y, SCREEN_W, 14, VC_GREEN);
    gfx->setTextSize(1);
    gfx->setTextColor(TFT_BLACK, VC_GREEN);
    gfx->setCursor(4, UI_CONTENT_Y + 3);
    gfx->print("WIFI PICKER");
    gfx->fillRect(0, UI_CONTENT_Y + 14, SCREEN_W, UI_CONTENT_H - 14, TFT_BLACK);

    if (wifiPhase == WPHASE_SCANNING) {
        gfx->setTextSize(2);
        gfx->setTextColor(TFT_CYAN, TFT_BLACK);
        gfx->setCursor(60, 80);
        gfx->print("Scanning...");
        int dots = (millis() / 400) % 4;
        gfx->setTextColor(TFT_YELLOW, TFT_BLACK);
        gfx->setCursor(60, 110);
        for (int d = 0; d < dots; d++) gfx->print(". ");
        drawButtonRow("--", "--", "--",
                      TFT_DARKGREY, TFT_DARKGREY, TFT_DARKGREY);

    } else if (wifiPhase == WPHASE_SELECT) {
        if (wifiScanCount == 0) {
            gfx->setTextSize(2);
            gfx->setTextColor(TFT_RED, TFT_BLACK);
            gfx->setCursor(40, 70);
            gfx->print("No networks");
            gfx->setTextSize(1);
            gfx->setTextColor(TFT_DARKGREY, TFT_BLACK);
            gfx->setCursor(40, 100);
            gfx->print("Press SCAN to retry");
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
                gfx->fillRect(0, ry, SCREEN_W, rowH - 1, bg);
                if (sel) {
                    gfx->fillRect(0, ry, 4, rowH - 1, VC_GREEN);
                    gfx->drawRect(0, ry, SCREEN_W, rowH - 1, VC_GREEN);
                }
                // SSID (truncate to 26 chars)
                gfx->setTextSize(1);
                gfx->setTextColor(sel ? VC_GREEN : TFT_WHITE, bg);
                gfx->setCursor(10, ry + 4);
                String ssid = wifiScanSSID[i];
                if (ssid.length() > 26) ssid = ssid.substring(0, 24) + "..";
                gfx->print(ssid);
                // Lock icon for secured networks
                if (!wifiScanOpen[i]) {
                    gfx->setTextColor(TFT_YELLOW, bg);
                    gfx->setCursor(10, ry + 16);
                    gfx->print("\xf0");  // lock-ish char
                }
                // RSSI bars
                drawRSSIBars(SCREEN_W - 30, ry + 6, wifiScanRSSI[i]);
                // dBm label
                gfx->setTextSize(1);
                gfx->setTextColor(TFT_DARKGREY, bg);
                char rssiLabel[8];
                snprintf(rssiLabel, sizeof(rssiLabel), "%d", (int)wifiScanRSSI[i]);
                gfx->setCursor(SCREEN_W - 60, ry + 10);
                gfx->print(rssiLabel);
            }

            // Scroll indicators
            if (wifiScrollTop > 0) {
                gfx->setTextColor(VC_GREEN, TFT_BLACK);
                gfx->setCursor(SCREEN_W / 2 - 6, y - 2);
                gfx->print("\x18");  // up arrow
            }
            if (endIdx < wifiScanCount) {
                gfx->setTextColor(VC_GREEN, TFT_BLACK);
                gfx->setCursor(SCREEN_W / 2 - 6, y + visibleRows * rowH);
                gfx->print("\x19");  // down arrow
            }

            // Count indicator
            gfx->setTextSize(1);
            gfx->setTextColor(TFT_DARKGREY, TFT_BLACK);
            gfx->setCursor(SCREEN_W - 50, UI_CONTENT_Y + 3);
            char countBuf[12];
            snprintf(countBuf, sizeof(countBuf), "%d/%d", wifiSelIdx + 1, wifiScanCount);
            gfx->print(countBuf);

            drawButtonRow("BACK", "SELECT", "NEXT",
                          VC_GREEN, TFT_GREEN, VC_GREEN);
        }

    } else if (wifiPhase == WPHASE_PASSWORD) {
        // ── Network name ────────────────────────────────────────────────
        gfx->setTextSize(1);
        gfx->setTextColor(VC_GREEN, TFT_BLACK);
        gfx->setCursor(4, UI_CONTENT_Y + 18);
        gfx->print("Network: ");
        gfx->setTextColor(TFT_WHITE, TFT_BLACK);
        String ssidDisp = wifiScanSSID[wifiSelIdx];
        if (ssidDisp.length() > 30) ssidDisp = ssidDisp.substring(0, 28) + "..";
        gfx->print(ssidDisp);

        // ── Password field ──────────────────────────────────────────────
        gfx->setTextColor(VC_GREEN, TFT_BLACK);
        gfx->setCursor(4, UI_CONTENT_Y + 34);
        gfx->print("Password:");
        // Show password with cursor
        gfx->setTextSize(1);
        gfx->setTextColor(TFT_CYAN, TFT_BLACK);
        gfx->setCursor(4, UI_CONTENT_Y + 48);
        // Show last 38 chars of password if it gets long
        String pwdShow = wifiPwdBuf;
        if (pwdShow.length() > 38) pwdShow = ".." + pwdShow.substring(pwdShow.length() - 36);
        gfx->print(pwdShow);
        // Blinking cursor
        if ((millis() / 500) % 2 == 0) {
            gfx->setTextColor(TFT_YELLOW, TFT_BLACK);
            gfx->print("_");
        }

        // ── Character picker ────────────────────────────────────────────
        // Show current char large, with neighbours on each side
        int cy = UI_CONTENT_Y + 72;
        gfx->fillRect(0, cy, SCREEN_W, 50, 0x0841u);  // subtle bg band

        // Neighbour chars (smaller, dimmed)
        gfx->setTextSize(2);
        for (int offset = -4; offset <= 4; offset++) {
            if (offset == 0) continue;
            int idx = ((int)wifiCharIdx + offset + WIFI_IDX_TOTAL) % WIFI_IDX_TOTAL;
            int px = SCREEN_W / 2 + offset * 28;
            if (px < 4 || px > SCREEN_W - 16) continue;
            gfx->setTextColor(TFT_DARKGREY, 0x0841u);
            gfx->setCursor(px - 6, cy + 16);
            if (idx == (int)WIFI_IDX_DONE) {
                gfx->setTextSize(1);
                gfx->print("OK");
                gfx->setTextSize(2);
            } else if (idx == (int)WIFI_IDX_DEL) {
                gfx->setTextSize(1);
                gfx->print("DEL");
                gfx->setTextSize(2);
            } else {
                gfx->print(WIFI_CHARS[idx]);
            }
        }

        // Current char — large and bright
        gfx->setTextSize(3);
        int cx = SCREEN_W / 2 - 9;
        if (wifiCharIdx == WIFI_IDX_DONE) {
            gfx->setTextColor(TFT_GREEN, 0x0841u);
            gfx->setCursor(cx - 18, cy + 10);
            gfx->setTextSize(2);
            gfx->print("[DONE]");
        } else if (wifiCharIdx == WIFI_IDX_DEL) {
            gfx->setTextColor(TFT_RED, 0x0841u);
            gfx->setCursor(cx - 15, cy + 10);
            gfx->setTextSize(2);
            gfx->print("[DEL]");
        } else {
            gfx->setTextColor(TFT_WHITE, 0x0841u);
            gfx->setCursor(cx, cy + 8);
            gfx->print(WIFI_CHARS[wifiCharIdx]);
        }

        // ── Hint line ───────────────────────────────────────────────────
        gfx->setTextSize(1);
        gfx->setTextColor(TFT_DARKGREY, TFT_BLACK);
        gfx->setCursor(10, UI_CONTENT_Y + 130);
        gfx->printf("Len: %d  |  PREV/NEXT char, ADD to append", wifiPwdBuf.length());

        // Category hint — show what region the char is in
        gfx->setCursor(10, UI_CONTENT_Y + 144);
        if (wifiCharIdx < 26) gfx->print("a-z lowercase");
        else if (wifiCharIdx < 52) gfx->print("A-Z uppercase");
        else if (wifiCharIdx < 62) gfx->print("0-9 digits");
        else if (wifiCharIdx < WIFI_CHAR_COUNT) gfx->print("symbols");
        else if (wifiCharIdx == WIFI_IDX_DONE) gfx->print("[DONE] = connect");
        else if (wifiCharIdx == WIFI_IDX_DEL) gfx->print("[DEL] = delete last char");

        drawButtonRow("PREV", "ADD", "NEXT",
                      VC_GREEN, TFT_GREEN, VC_GREEN);

    } else if (wifiPhase == WPHASE_CONNECTING) {
        gfx->setTextSize(2);
        gfx->setTextColor(TFT_CYAN, TFT_BLACK);
        gfx->setCursor(40, 70);
        gfx->print("Connecting...");
        gfx->setTextSize(1);
        gfx->setTextColor(TFT_WHITE, TFT_BLACK);
        gfx->setCursor(40, 100);
        gfx->print(wifiScanSSID[wifiSelIdx]);
        int dots = (millis() / 400) % 4;
        gfx->setTextColor(TFT_YELLOW, TFT_BLACK);
        gfx->setCursor(40, 120);
        for (int d = 0; d < dots; d++) gfx->print(". ");
        drawButtonRow("--", "--", "--",
                      TFT_DARKGREY, TFT_DARKGREY, TFT_DARKGREY);

    } else {  // WPHASE_RESULT
        gfx->setTextSize(2);
        uint16_t resCol = wifiResultOK ? TFT_GREEN : TFT_RED;
        gfx->setTextColor(resCol, TFT_BLACK);
        gfx->setCursor(20, 50);
        gfx->print(wifiResultOK ? "CONNECTED" : "FAILED");
        gfx->setTextSize(1);
        gfx->setTextColor(TFT_WHITE, TFT_BLACK);
        gfx->setCursor(20, 80);
        gfx->print(wifiResultMsg);
        if (wifiResultOK) {
            gfx->setTextColor(TFT_CYAN, TFT_BLACK);
            gfx->setCursor(20, 100);
            gfx->print("IP: ");
            gfx->print(WiFi.localIP().toString());
            gfx->setCursor(20, 116);
            gfx->printf("RSSI: %d dBm", wifiRSSI);
        }
        // Auto-return timer bar
        unsigned long elapsed = millis() - wifiResultMs;
        if (elapsed < TOOLS_RESULT_TIMEOUT_MS) {
            float frac = (float)elapsed / (float)TOOLS_RESULT_TIMEOUT_MS;
            int barW = (int)((1.0f - frac) * (SCREEN_W - 20));
            gfx->fillRect(10, 140, barW, 3, resCol);
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
            if (btnAPressed()) {
                wifiPhase = WPHASE_SCANNING;  // re-scan triggered in loop()
            }
            if (btnCPressed()) {
                exitWifiPicker(SCREEN_CONFIG);
            }
        } else {
            if (btnAPressed()) {
                // BACK — return to config without connecting
                exitWifiPicker(SCREEN_CONFIG);
            }
            if (btnCPressed()) {
                // NEXT (wraps, so all networks reachable without PREV)
                wifiSelIdx = (wifiSelIdx + 1) % wifiScanCount;
            }
            if (btnBPressed()) {
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
        if (btnAPressed()) {
            // PREV char
            if (wifiCharIdx == 0) wifiCharIdx = WIFI_IDX_TOTAL - 1;
            else wifiCharIdx--;
        }
        if (btnCPressed()) {
            // NEXT char
            wifiCharIdx = (wifiCharIdx + 1) % WIFI_IDX_TOTAL;
        }
        if (btnBPressed()) {
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
        if (btnAPressed()) {
            // SCAN again
            wifiPhase = WPHASE_SCANNING;
        }
        if (btnCPressed()) {
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
    // Point all draw helpers at the off-screen canvas (if available),
    // otherwise fall back to drawing directly to the display.
    bool useCanvas = frameCanvas.getBuffer() != nullptr;
    gfx = useCanvas ? (lgfx::LovyanGFX*)&frameCanvas : (lgfx::LovyanGFX*)&M5.Display;

    drawHazard(0, 0, SCREEN_W, UI_TOP_H);
    switch (currentScreen) {
        case SCREEN_HOME:     drawHomeScreen();     break;
        case SCREEN_STATUS:   drawStatusScreen();   break;
        case SCREEN_CONFIG:   drawConfigScreen();   break;
        case SCREEN_TOOLS:    drawToolsScreen();    break;
        case SCREEN_DURATION: drawDurationScreen(); break;
        case SCREEN_WIFI:     drawWifiScreen();     break;
    }

    // Push completed frame to the physical display in one transfer.
    // The user never sees intermediate black-clear states.
    if (useCanvas) frameCanvas.pushSprite(0, 0);

    gfx = &M5.Display;  // restore for any direct M5.Display calls outside updateDisplay()
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
        if (btnAPressed()) {
            toolsSelectedItem = (toolsSelectedItem + TOOL_COUNT - 1) % TOOL_COUNT;
        }
        if (btnBPressed()) {
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
        if (btnCPressed()) {
            currentScreen = SCREEN_HOME;
        }
        break;

    case TPHASE_CONFIRM:
        if (btnBPressed()) {
            Serial.println("[tools] User-initiated restart");
            delay(200);
            ESP.restart();
        }
        if (btnCPressed()) {
            toolsPhase    = TPHASE_SELECT;
            currentScreen = SCREEN_HOME;
        }
        break;

    case TPHASE_RUNNING:
        // no input while running
        break;

    case TPHASE_RESULT:
        if (btnBPressed()) toolsPhase = TPHASE_SELECT;
        if (btnCPressed()) {
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
    bool anyPressed = btnAPressed() || btnBPressed() || btnCPressed();
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
            if (btnAPressed()) {
                // RUN — same logic as old BtnA handler
                checkWiFi();
                if (shouldUseSdStream()) {
                    // SD-streaming path — no PSRAM audio buffers needed
                    continuousMode   = true;
                    stopContinuous   = false;
                    continuousChunks = 0;
                    startRecording();
                } else if (!audioBuffer && !allocateAudioBuffer()) {
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
            if (btnBPressed()) currentScreen = SCREEN_CONFIG;
            if (btnCPressed()) currentScreen = SCREEN_TOOLS;
        } else if (activeRec) {
            if (btnBPressed()) {
                // STOP — same logic as old BtnB handler
                if (appState == STATE_CONTINUOUS) {
                    stopContinuous = true;
                    lastStatus = "Stopping after this chunk...";
                } else if (appState == STATE_RECORDING) {
                    if (sdStreamMode) {
                        if (sdRecSamples > 0) {
                            finishSdRecording();
                            appState   = STATE_ENCODING;
                            lastStatus = "Stopped early ~ uploading...";
                        } else {
                            M5.Mic.end();
                            isRecording  = false;
                            sdStreamMode = false;
                            appState     = STATE_IDLE;
                            lastStatus   = "Recording cancelled";
                        }
                    } else {
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
            }
            if (btnCPressed()) currentScreen = SCREEN_STATUS;
        } else {
            // ENCODING / UPLOADING
            if (btnCPressed()) currentScreen = SCREEN_STATUS;
        }
        break;

    case SCREEN_STATUS:
        if (btnAPressed()) currentScreen = SCREEN_HOME;
        break;

    case SCREEN_CONFIG:
        if (btnAPressed()) currentScreen = SCREEN_HOME;
        if (btnBPressed()) {
            durEditPending = recordDurationSec;   // seed picker with live value
            currentScreen  = SCREEN_DURATION;
        }
        if (btnCPressed()) {
            wifiWasConnected = wifiConnected;
            wifiPhase        = WPHASE_SCANNING;
            currentScreen    = SCREEN_WIFI;
        }
        break;

    case SCREEN_DURATION: {
        if (btnAPressed()) {
            // LESS — decrease by 5 s, clamp at minimum
            if (durEditPending > MIN_RECORD_DURATION_SEC) {
                uint32_t s = (durEditPending <= 120) ? 5 : 60;
                uint32_t next = (durEditPending > s) ? durEditPending - s : MIN_RECORD_DURATION_SEC;
                durEditPending = (next < MIN_RECORD_DURATION_SEC)
                                 ? MIN_RECORD_DURATION_SEC : next;
            }
        }
        if (btnCPressed()) {
            // MORE — increase by 5 s, clamp at maximum
            if (durEditPending < MAX_RECORD_DURATION_SEC) {
                uint32_t s = (durEditPending >= 120) ? 60 : 5;
                uint32_t next = durEditPending + s;
                durEditPending = (next > MAX_RECORD_DURATION_SEC)
                                 ? MAX_RECORD_DURATION_SEC : next;
            }
        }
        if (btnBPressed()) {
            // SAVE — commit and free old buffers so they reallocate at new size
            if (appState == STATE_RECORDING || appState == STATE_CONTINUOUS ||
                appState == STATE_ENCODING  || appState == STATE_UPLOADING) {
                // Can't change mid-recording — silently ignore
            } else {
                recordDurationSec = durEditPending;
                freeAudioBuffers();
                saveConfig();
                const char* mode;
                if (shouldUseSdStream()) mode = useMp3 ? "SD/MP3" : "SD/WAV";
                else if (recordDurationSec <= CONT_MAX_DURATION_SEC) mode = "continuous";
                else mode = "single-shot";
                Serial.printf("[dur] Duration set to %u s (%s)\n",
                              recordDurationSec, mode);
            }
            currentScreen = SCREEN_CONFIG;
        }
        break;
    }

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
            {
                const char* mode;
                if (shouldUseSdStream()) mode = useMp3 ? "SD/MP3" : "SD/WAV";
                else if (recordDurationSec <= CONT_MAX_DURATION_SEC) mode = "continuous";
                else mode = "single-shot";
                Serial.printf("[cmd] Recording duration: %u s (%s mode)\n",
                              recordDurationSec, mode);
            }
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
                    if (shouldUseSdStream()) {
                        Serial.printf(" (SD-streaming, %s)\n",
                                      useMp3 ? "MP3" : "WAV");
                    } else if (recordDurationSec <= CONT_MAX_DURATION_SEC) {
                        Serial.println(" (continuous mode available)");
                    } else {
                        Serial.println(" (single-shot PSRAM)");
                    }
                    Serial.println("[cmd] Buffers freed — will reallocate on next RUN press");
                }
            }
        }
    } else if (cmd.startsWith("mp3") || cmd.startsWith("sdstream")) {
        // mp3 on / mp3 off / mp3 (show status)
        // sdstream on / sdstream off / sdstream (show status)
        bool isMp3Cmd = cmd.startsWith("mp3");
        String arg = cmd.substring(isMp3Cmd ? 3 : 8);
        arg.trim();

        if (isMp3Cmd) {
#if ENABLE_MP3
            if (arg == "on")  { useMp3 = true;  saveConfig(); Serial.println("[cmd] MP3 encoding ON"); }
            else if (arg == "off") { useMp3 = false; saveConfig(); Serial.println("[cmd] MP3 encoding OFF (WAV)"); }
            else { Serial.printf("[cmd] MP3 encoding: %s\n", useMp3 ? "ON" : "OFF"); }
#else
            Serial.println("[cmd] MP3 not compiled — set ENABLE_MP3=1 in config.h");
#endif
        } else {
            if (arg == "on")  { sdStreamForce = true;  Serial.println("[cmd] SD-streaming forced ON"); }
            else if (arg == "off") { sdStreamForce = false; Serial.println("[cmd] SD-streaming auto"); }
            else { Serial.printf("[cmd] SD-streaming: %s\n",
                                 sdStreamForce ? "forced ON" : "auto (>45s)"); }
        }
    } else if (cmd == "status") {
        Serial.println("\n=== vCon Recorder Status ===");
        Serial.printf("Firmware:    %s\n", FIRMWARE_VERSION);
        Serial.printf("Device ID:   %s\n", deviceID.c_str());
        Serial.printf("MAC Address: %s\n", WiFi.macAddress().c_str());
        {
            const char* mode;
            if (shouldUseSdStream()) mode = useMp3 ? "SD/MP3" : "SD/WAV";
            else if (recordDurationSec <= CONT_MAX_DURATION_SEC) mode = "continuous";
            else mode = "single-shot";
            Serial.printf("Rec Duration:%u s (%s, range %u-%u s)\n",
                          recordDurationSec, mode,
                          MIN_RECORD_DURATION_SEC, MAX_RECORD_DURATION_SEC);
        }
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
#if ENABLE_MP3
        Serial.printf("MP3 Encode:  %s\n", useMp3 ? "ON" : "OFF");
#endif
        Serial.printf("SD Stream:   %s\n", sdStreamForce ? "forced ON" : "auto");
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
        Serial.printf("\n=== %s vCon Recorder ===\n", BOARD_NAME);
        Serial.println("wifi  <ssid> <password>  Set WiFi credentials");
        Serial.println("url   <post_url>          Set vCon POST URL");
        Serial.println("token <dvt_xxx>           Set portal device token (token routing)");
        Serial.println("token                     Clear token (use MAC routing)");
        Serial.printf( "dur   <10-%u>           Set recording duration in seconds\n",
                       MAX_RECORD_DURATION_SEC);
        Serial.println("dur                       Show current recording duration");
        Serial.println("mp3 on/off                Toggle MP3 compression (SD-stream)");
        Serial.println("mp3                       Show MP3 status");
        Serial.println("sdstream on/off           Force SD-streaming mode on/off");
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
    Serial.printf("\n\n=== %s vCon Recorder (VConic) ===\n", BOARD_NAME);
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

    // PSRAM check — required for the audio buffer (both Core2 and CoreS3 have 8 MB)
    if (!psramFound()) {
        M5.Display.fillScreen(TFT_RED);
        M5.Display.setTextColor(TFT_WHITE, TFT_RED);
        M5.Display.setCursor(20, 80);
        M5.Display.setTextSize(2);
        M5.Display.print("FATAL: No PSRAM");
        M5.Display.setCursor(20, 110);
        M5.Display.print("This firmware");
        M5.Display.setCursor(20, 130);
        M5.Display.print("requires PSRAM");
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

#if HAS_CAMERA
    initCamera();
#endif

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

    // Allocate full-screen canvas from PSRAM (320×240×2 = 153 600 bytes).
    // Must be created before the first updateDisplay() call.
    frameCanvas.setColorDepth(16);
    if (!frameCanvas.createSprite(SCREEN_W, SCREEN_H)) {
        Serial.println("[display] WARNING: frameCanvas alloc failed — drawing direct");
    } else {
        Serial.printf("[display] frameCanvas ready (%d bytes)\n", SCREEN_W * SCREEN_H * 2);
    }

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
    updateTouchButtons();
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
