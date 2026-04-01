#ifndef CONFIG_H
#define CONFIG_H

// =============================================================================
// M5Stack Core2 vCon Recorder — Configuration
// =============================================================================
// Edit defaults here, or use the serial interface to change at runtime.
// All settings are persisted to flash via Preferences.
//
// Serial Commands (115200 baud):
//   wifi <ssid> <password>  — Set WiFi credentials
//   url  <post_url>         — Set vCon POST endpoint
//   status                  — Show current configuration
//   restart                 — Reboot device
//   help                    — List commands

// WiFi defaults
#define DEFAULT_WIFI_SSID     "barnhill tavern"
#define DEFAULT_WIFI_PASSWORD "meteormeteor"

// vCon POST endpoint default
#define DEFAULT_POST_URL      "https://vcon-gateway.replit.app/ingress"

// Firmware version — must match /public/version.txt on the OTA server
// when this build is the current release.
#define FIRMWARE_VERSION      "1.0.5"

// OTA endpoints — served as static files from the same Replit host
#define OTA_VERSION_URL       "https://vcon-gateway.replit.app/api/ota/version.txt"
#define OTA_FIRMWARE_URL      "https://vcon-gateway.replit.app/api/ota/firmware.bin"

// VConic portal device token (optional).
// When set, appended as ?token=<value> and sent as X-Device-Token header.
// Leave empty to fall back to MAC-address routing.
// Obtain from: Portal → Devices → [your device] → Gateway URL (recommended)
#define DEFAULT_DEVICE_TOKEN  ""

// Audio
#define SAMPLE_RATE           8000   // Hz — telephone quality, fits in PSRAM
#define BITS_PER_SAMPLE       16     // 16-bit signed PCM

// Recording duration — default set here, overridden at runtime via 'dur' serial command.
// Stored in flash (Preferences key "rec_dur") and survives power cycles.
#define RECORD_DURATION_SEC     45   // default: 45 s per chunk (continuous mode)
#define MIN_RECORD_DURATION_SEC 10   // shortest allowed (codec settle overhead)
#define MAX_RECORD_DURATION_SEC 120  // longest allowed (PSRAM budget; see note below)

// Continuous dual-buffer mode requires two full PCM buffers in PSRAM simultaneously.
// At 60 s the two PCM buffers (2 × 960 KB) consume ~1.92 MB, leaving only ~2.1 MB
// for the JSON buffer (~1.22 MB) + WAV staging (~960 KB) — about 49 KB short.
// 45 s gives a comfortable ~800 KB margin: encode peak ~1.68 MB vs ~2.67 MB free.
// Above this threshold the firmware automatically falls back to single-shot mode.
#define CONT_MAX_DURATION_SEC   45

// ---- Buffer sizes are now computed at runtime from recordDurationSec ----
// Use the inline helpers audioSampleTarget() / audioPcmBytes() in the sketch.
//
// Memory budget reference (at D seconds, 8 kHz/16-bit mono):
//   PCM buffer        : D × 16 000 B
//   WAV staging       : D × 16 000 + 44 B
//   base64url body    : D × 21 340 B  (≈ D × 16 000 × 4/3)
//   Full JSON         : base64url + ~1 000 B prefix/suffix
//
//   Single-shot peak  : D × 53.3 KB   →  60 s ≈ 3.2 MB  ✓  120 s ≈ 6.4 MB  ✓/⚠
//   Continuous peak   : D × 69.3 KB   →  60 s ≈ 4.2 MB  ✓  > 60 s exceeds ~4 MB  ✗
//
// Core2 has 8 MB PSRAM; ~4–5 MB is available after IDF/WiFi/mbedtls overhead.

// Display (M5Stack Core2 built-in LCD)
#define SCREEN_W  320
#define SCREEN_H  240

#endif // CONFIG_H
