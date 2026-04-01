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
#define FIRMWARE_VERSION      "1.0.0"

// OTA endpoints — served as static files from the same Replit host
#define OTA_VERSION_URL       "https://vcon-gateway.replit.app/version.txt"
#define OTA_FIRMWARE_URL      "https://vcon-gateway.replit.app/firmware.bin"

// VConic portal device token (optional).
// When set, appended as ?token=<value> and sent as X-Device-Token header.
// Leave empty to fall back to MAC-address routing.
// Obtain from: Portal → Devices → [your device] → Gateway URL (recommended)
#define DEFAULT_DEVICE_TOKEN  ""

// Audio
#define SAMPLE_RATE           8000   // Hz — telephone quality, fits in PSRAM
#define BITS_PER_SAMPLE       16     // 16-bit signed PCM
#define RECORD_DURATION_SEC   15     // 15 seconds per recording

// Display (M5Stack Core2 built-in LCD)
#define SCREEN_W  320
#define SCREEN_H  240

// ---- Computed sizes (do not edit) ----
// Raw PCM audio  : 8000 Hz × 15 s × 2 bytes = 240 000 bytes
// WAV file       : PCM + 44-byte header      = 240 044 bytes
// base64url body : ceil(240044 / 3) × 4      =  320 060 bytes
// Full JSON      : prefix + b64 + suffix      ≈  321 000 bytes
// Core2 PSRAM is 8 MB — all buffers fit comfortably.

#define AUDIO_SAMPLES     ((uint32_t)SAMPLE_RATE * RECORD_DURATION_SEC)  // 120 000  @ 15 s
#define AUDIO_PCM_BYTES   (AUDIO_SAMPLES * 2u)                           // 240 000 bytes
#define WAV_TOTAL_BYTES   (44u + AUDIO_PCM_BYTES)                        // 240 044 bytes
#define B64_OUTPUT_SIZE   (((WAV_TOTAL_BYTES + 2u) / 3u * 4u) + 16u)    // ≥ 320 076 bytes

#endif // CONFIG_H
