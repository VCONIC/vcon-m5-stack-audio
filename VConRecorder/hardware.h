#ifndef HARDWARE_H
#define HARDWARE_H

// =============================================================================
// Board-specific hardware abstraction
// =============================================================================
// Compile-time detection using Arduino ESP32 core defines.
// The correct define is set automatically by the FQBN passed to arduino-cli:
//   esp32:esp32:m5stack_core2  → ARDUINO_M5STACK_CORE2
//   esp32:esp32:m5stack_cores3 → ARDUINO_M5STACK_CORES3
//
// SPI pin mappings MUST be resolved at compile time — they are needed before
// M5.begin() is called, so runtime detection via M5.getBoard() is not an option.
// =============================================================================

#if defined(ARDUINO_M5STACK_CORES3)

  // ---- M5Stack CoreS3 -------------------------------------------------------
  #define BOARD_NAME        "M5Stack CoreS3"
  #define DEVICE_TYPE_STR   "m5stack-cores3"

  // SD card — SPI2 bus (different GPIO mapping from Core2)
  #define SD_PIN_CLK   36
  #define SD_PIN_MISO  35
  #define SD_PIN_MOSI  37
  #define SD_PIN_CS     4

  // OTA — board-specific endpoints so Core2 and CoreS3 never cross-flash
  #define OTA_VERSION_URL   "https://vcon-gateway.replit.app/api/ota/cores3/version.txt"
  #define OTA_FIRMWARE_URL  "https://vcon-gateway.replit.app/api/ota/cores3/firmware.bin"

  // Camera (GC0308) is available on CoreS3
  #define HAS_CAMERA  1

#elif defined(ARDUINO_M5STACK_CORE2)

  // ---- M5Stack Core2 (default) ----------------------------------------------
  #define BOARD_NAME        "M5Stack Core2"
  #define DEVICE_TYPE_STR   "m5stack-core2"

  // SD card — VSPI bus
  #define SD_PIN_CLK   18
  #define SD_PIN_MISO  38
  #define SD_PIN_MOSI  23
  #define SD_PIN_CS     4

  // OTA — board-specific endpoints
  #define OTA_VERSION_URL   "https://vcon-gateway.replit.app/api/ota/core2/version.txt"
  #define OTA_FIRMWARE_URL  "https://vcon-gateway.replit.app/api/ota/core2/firmware.bin"

  // No camera on Core2
  #define HAS_CAMERA  0

#else
  #error "Unsupported board. Build with --fqbn esp32:esp32:m5stack_core2 or esp32:esp32:m5stack_cores3"
#endif

#endif // HARDWARE_H
