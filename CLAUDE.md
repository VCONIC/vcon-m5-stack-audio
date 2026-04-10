# CLAUDE.md

## Project Overview

**M5Stack vCon Audio Recorder** — an ESP32 Arduino firmware that records audio from the built-in microphone and publishes each chunk as a spec-compliant [vCon](https://datatracker.ietf.org/doc/draft-ietf-vcon-vcon-core/) JSON document (IETF draft-ietf-vcon-vcon-core v0.4.0) to the VConic gateway. Recordings are also saved to microSD card. On CoreS3, a camera thumbnail is captured at each recording start and included as a vCon attachment.

- **Hardware:** M5Stack Core2 (ESP32-D0WDQ6-V3) and M5Stack CoreS3 (ESP32-S3) — both dual-core 240 MHz, 8 MB PSRAM
- **Audio:** 8 kHz / 16-bit mono PCM, base64url-encoded WAV in vCon JSON
- **Camera (CoreS3 only):** GC0308 QVGA JPEG thumbnail, base64url-encoded in vCon attachments
- **Current firmware version:** defined in `VConRecorder/config.h` as `FIRMWARE_VERSION`

## Repository Structure

```
vcon-m5-stack-audio/
├── CLAUDE.md                         ← This file
├── README.md                         ← Full user/developer documentation
├── USER_GUIDE.md                     ← End-user guide
├── CAPTIVE_PORTAL_IMPLEMENTATION_PLAN.md  ← Future feature plan
├── test_device.sh                    ← Device integration tests (pyserial over serial)
├── .gitignore
└── VConRecorder/                     ← Arduino sketch directory
    ├── VConRecorder.ino              ← Main sketch (single-file application logic)
    ├── config.h                      ← Compile-time defaults, buffer math
    ├── hardware.h                    ← Board detection, SD pins, OTA URLs, device strings
    ├── ota.h                         ← OTA update logic (HTTP poll + ESP32 Update lib)
    ├── logo.h                        ← VConic logo bitmap (RGB565, PROGMEM)
    └── build/                        ← Compiled output (firmware.bin kept in git)
        ├── esp32.esp32.m5stack_core2/    ← Core2 firmware
        └── esp32.esp32.m5stack_cores3/   ← CoreS3 firmware
```

## Build System

This is an **Arduino CLI** project (not PlatformIO). No `platformio.ini` exists.

### Compile

Build for a specific board by setting the FQBN:

```bash
# Core2
arduino-cli compile \
  --fqbn esp32:esp32:m5stack_core2 \
  --build-property "build.partitions=min_spiffs" \
  --build-property "upload.maximum_size=1966080" \
  --output-dir VConRecorder/build/esp32.esp32.m5stack_core2 \
  VConRecorder/VConRecorder.ino

# CoreS3
arduino-cli compile \
  --fqbn esp32:esp32:m5stack_cores3 \
  --build-property "build.partitions=min_spiffs" \
  --build-property "upload.maximum_size=1966080" \
  --output-dir VConRecorder/build/esp32.esp32.m5stack_cores3 \
  VConRecorder/VConRecorder.ino
```

Board detection is automatic via `hardware.h` — the Arduino ESP32 core defines `ARDUINO_M5STACK_CORE2` or `ARDUINO_M5STACK_CORES3` based on the FQBN.

### Flash

```bash
arduino-cli upload \
  --fqbn esp32:esp32:m5stack_core2 \
  --port <serial_port> \
  --input-dir VConRecorder/build/esp32.esp32.m5stack_core2 \
  VConRecorder/VConRecorder.ino
```

(Substitute `m5stack_cores3` for CoreS3.)

### Dependencies

- **ESP32 Arduino core** (`esp32:esp32`) v3.x
- **M5Unified** library v0.2.13+
- Partition scheme: **Minimal SPIFFS (1.9 MB APP with OTA)** — required for OTA

## Claude Code Skills

The repo ships custom Claude Code skills for device workflows:

| Skill | Trigger phrase | What it does |
|---|---|---|
| `flash-controller` | *"flash the device"*, *"update the M5"*, *"reflash"* | Auto-discovers the USB serial port, asks which board (Core2/CoreS3), reads the running firmware version via `status` serial command, compares against `config.h`, flashes the correct pre-built binary, and verifies the new version boots correctly |
| `release-firmware` | *"release"*, *"cut a release"*, *"bump and deploy"* | Increments `FIRMWARE_VERSION` in `config.h` (patch by default; say "minor" or "major" to override), compiles **both** board variants, commits + pushes to git, then — after explicit confirmation — authenticates with the OTA gateway and deploys both binaries and version strings in the correct order |

Skill definitions: `.claude/skills/flash-controller/SKILL.md`, `.claude/skills/release-firmware/SKILL.md`

> **Note for contributors:** `.claude/settings.local.json` is gitignored — it holds machine-specific tool permissions accumulated during your Claude Code sessions and should not be committed.

## Architecture

### Single-file sketch

All application logic lives in `VConRecorder.ino`. Headers provide:
- `config.h` — compile-time defaults, buffer math
- `hardware.h` — board-specific constants (SD pins, OTA URLs, device type, camera flag)
- `ota.h` — OTA update logic
- `logo.h` — splash screen bitmap

### Hardware abstraction (`hardware.h`)

Board detection uses compile-time `#ifdef ARDUINO_M5STACK_CORES3` / `ARDUINO_M5STACK_CORE2`. This provides:
- SD card SPI pin mapping (different GPIOs per board)
- OTA endpoint URLs (board-specific paths prevent cross-flash)
- Device type string for vCon JSON metadata
- `HAS_CAMERA` flag (1 on CoreS3, 0 on Core2)

Runtime abstraction is handled by M5Unified (display, mic, buttons, power management).

### State machine

```
STATE_IDLE → STATE_RECORDING → STATE_ENCODING → STATE_UPLOADING → STATE_SUCCESS/STATE_ERROR
                                                                        ↓
STATE_CONTINUOUS (dual-buffer zero-gap mode, ≤45s chunks)          back to IDLE
```

### Dual-core usage

- **Core 1 (main loop):** mic DMA recording, display updates, button/touch events, serial commands
- **Core 0 (FreeRTOS task):** base64 encoding, SD card writes, HTTP POST (continuous mode only)

Thread safety: the upload task on core 0 only writes to `volatile` primitives. All String/display operations stay on core 1.

### Memory model

All large buffers are allocated in **PSRAM** (8 MB external, ~4 MB usable). Key constraint: continuous dual-buffer mode requires two PCM buffers + encoding space, limiting it to ≤45s chunks (`CONT_MAX_DURATION_SEC`).

On CoreS3, the camera snapshot adds ~15-25 KB of PSRAM usage per recording — negligible compared to the audio buffers.

### Camera (CoreS3 only)

The GC0308 camera is initialized at boot via `esp_camera`. A single QVGA (320×240) JPEG snapshot is captured at the start of each recording and included as a base64url-encoded `"thumbnail"` attachment in the vCon JSON. The snapshot is captured before mic DMA starts to avoid bus contention.

### UI system

Four main screens plus a DURATION sub-screen, all sharing a top hazard strip and bottom button row:
- **HOME** — connection status, state, stats
- **STATUS** — real-time VU meter, upload progress
- **CONFIG** — read-only settings display
- **TOOLS** — diagnostic menu (WiFi test, OTA check, POST test, SD info, restart)
- **DURATION** — interactive duration picker (sub-screen of TOOLS)

Both Core2 (capacitive touch buttons below screen) and CoreS3 (touchscreen zones) use M5Unified's `BtnA`/`BtnB`/`BtnC` abstraction.

### Key functions

| Function | Purpose |
|----------|---------|
| `loadConfig()` / `saveConfig()` | NVS flash persistence (WiFi, URL, token, counters) |
| `connectWiFi()` | Full WiFi stack reset + connect |
| `buildAndUploadVConCore()` | Encode WAV → base64url → vCon JSON (+ optional thumbnail) → SD save → HTTP POST |
| `uploadTaskFn()` | FreeRTOS task for background encode/upload (core 0) |
| `startRecording()` / `recordAudioChunk()` | Mic DMA capture (+ camera snapshot on CoreS3) |
| `initCamera()` / `captureSnapshot()` | CoreS3 only: GC0308 init and JPEG capture |
| `updateDisplay()` | Screen dispatcher |
| `handleButtons()` | Central button/touch dispatcher |

## Configuration

All compile-time defaults are in `VConRecorder/config.h`. Board-specific constants (SD pins, OTA URLs) are in `VConRecorder/hardware.h`. Runtime overrides are persisted to NVS flash via the `Preferences` library and take precedence.

Key settings: WiFi SSID/password, POST URL, device token, recording duration (10-120s), sample rate (8 kHz), firmware version.

Serial commands at 115200 baud: `wifi`, `url`, `token`, `dur`, `status`, `scan`, `sd`, `restart`, `help`.

## Testing

`test_device.sh` runs 17 integration tests over a serial connection using pyserial. It requires a physical device connected via USB:

```bash
./test_device.sh /dev/cu.usbserial-XXXXX
```

There are no unit tests or CI pipelines — testing is device-based.

## OTA Updates

The device checks for firmware updates on every boot after WiFi connects. Each board variant has its own OTA endpoint:
- Core2: `vcon-gateway.replit.app/api/ota/core2/`
- CoreS3: `vcon-gateway.replit.app/api/ota/cores3/`

Deploy order matters: upload `firmware.bin` before updating `version.txt`. Both boards share the same `FIRMWARE_VERSION` string.

## Code Conventions

- **Language:** C++ (Arduino dialect), single `.ino` file with `.h` headers
- **Naming:** `camelCase` for functions and local variables, `UPPER_SNAKE` for `#define` constants, `g_` prefix for global task handles
- **Board guards:** `#if HAS_CAMERA` for CoreS3-only camera code; `#ifdef ARDUINO_M5STACK_CORES3` in `hardware.h`
- **State:** enum-based state machine (`AppState`, `UIScreen`, `ToolsPhase`)
- **Concurrency:** `volatile` for cross-core shared primitives, no mutexes
- **Memory:** PSRAM for large buffers (`ps_malloc`), heap for small allocations
- **Display:** M5Unified `M5.Display` API with an off-screen `M5Canvas` sprite for flicker-free VU meter
- **Color:** VConic brand green `#30CC30` → RGB565 `0x3666` (`VC_GREEN`)
- **Serial logging:** prefixed tags like `[wifi]`, `[sd]`, `[OTA]`, `[ntp]`, `[cam]`
- **Error handling:** retry with exponential backoff for HTTP POST (3 retries: 5s → 30s → 5min)

## Important Constraints

- **PSRAM budget is tight.** Any buffer changes must account for the memory math in `config.h`. Continuous mode peaks at ~2.9 MB for 45s chunks with only ~800 KB margin.
- **Partition scheme must be `min_spiffs`** for OTA to work. Default partition scheme will cause `Update.begin()` to fail.
- **WiFi SSID parsing:** the `wifi` serial command treats the last word as the password; everything before it is the SSID (supports spaces in SSIDs).
- **`FIRMWARE_VERSION` in `config.h` must match the OTA server's `version.txt`** for the running release, or devices will re-flash on every boot.
- **The `.gitignore` keeps `firmware.bin` in the repo** but excludes other build artifacts.
- **OTA endpoints are board-specific.** Core2 and CoreS3 firmware binaries are NOT interchangeable — flashing the wrong binary will brick the device until manual re-flash.
- **Camera pin mapping (CoreS3)** is hardcoded in `VConRecorder.ino::initCamera()`. If a future CoreS3 revision changes the GC0308 wiring, update the pin constants there.
