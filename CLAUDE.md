# CLAUDE.md

## Project Overview

**M5Stack Core2 vCon Audio Recorder** — an ESP32 Arduino firmware that records audio from the M5Stack Core2's built-in PDM microphone and publishes each chunk as a spec-compliant [vCon](https://datatracker.ietf.org/doc/draft-ietf-vcon-vcon-core/) JSON document (IETF draft-ietf-vcon-vcon-core v0.4.0) to the VConic gateway. Recordings are also saved to microSD card.

- **Hardware:** M5Stack Core2 (ESP32-D0WDQ6-V3, dual-core 240 MHz, 8 MB PSRAM)
- **Audio:** 8 kHz / 16-bit mono PCM, base64url-encoded WAV in vCon JSON
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
    ├── VConRecorder.ino              ← Main sketch (~2500 lines, single-file)
    ├── config.h                      ← Compile-time defaults, buffer math, OTA URLs
    ├── ota.h                         ← OTA update logic (HTTP poll + ESP32 Update lib)
    ├── logo.h                        ← VConic logo bitmap (RGB565, PROGMEM)
    └── build/                        ← Compiled output (firmware.bin kept in git)
        └── esp32.esp32.m5stack_core2/
```

## Build System

This is an **Arduino CLI** project (not PlatformIO). No `platformio.ini` exists.

### Compile

```bash
arduino-cli compile \
  --fqbn esp32:esp32:m5stack_core2 \
  --build-property "build.partitions=min_spiffs" \
  --build-property "upload.maximum_size=1966080" \
  --output-dir VConRecorder/build \
  VConRecorder/VConRecorder.ino
```

### Flash

```bash
arduino-cli upload \
  --fqbn esp32:esp32:m5stack_core2 \
  --port <serial_port> \
  --input-dir VConRecorder/build \
  VConRecorder/VConRecorder.ino
```

### Dependencies

- **ESP32 Arduino core** (`esp32:esp32`) v3.x
- **M5Unified** library v0.2.13+
- Partition scheme: **Minimal SPIFFS (1.9 MB APP with OTA)** — required for OTA

## Claude Code Skills

The repo ships a custom Claude Code skill for device workflows:

| Skill | Trigger phrase | What it does |
|---|---|---|
| `flash-controller` | *"flash the device"*, *"update the M5"*, *"reflash"* | Auto-discovers the USB serial port, reads the running firmware version via `status` serial command, compares against `config.h`, compiles if asked, flashes the pre-built binary, and verifies the new version boots correctly |
| `release-firmware` | *"release"*, *"cut a release"*, *"bump and deploy"* | Increments `FIRMWARE_VERSION` in `config.h` (patch by default; say "minor" or "major" to override), compiles, commits + pushes to git, then — after explicit confirmation — authenticates with the OTA gateway and deploys the binary and version string in the correct order |

Skill definitions: `.claude/skills/flash-controller/SKILL.md`, `.claude/skills/release-firmware/SKILL.md`

> **Note for contributors:** `.claude/settings.local.json` is gitignored — it holds machine-specific tool permissions accumulated during your Claude Code sessions and should not be committed.

## Architecture

### Single-file sketch

All application logic lives in `VConRecorder.ino` (~2500 lines). Headers provide configuration (`config.h`), OTA logic (`ota.h`), and the splash logo (`logo.h`).

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

### UI system

Four main screens plus a DURATION sub-screen, all sharing a top hazard strip and bottom button row:
- **HOME** — connection status, state, stats
- **STATUS** — real-time VU meter, upload progress
- **CONFIG** — read-only settings display
- **TOOLS** — diagnostic menu (WiFi test, OTA check, POST test, SD info, restart)
- **DURATION** — interactive duration picker (sub-screen of TOOLS)

### Key functions

| Function | Purpose |
|----------|---------|
| `loadConfig()` / `saveConfig()` | NVS flash persistence (WiFi, URL, token, counters) |
| `connectWiFi()` | Full WiFi stack reset + connect |
| `buildAndUploadVConCore()` | Encode WAV → base64url → vCon JSON → SD save → HTTP POST |
| `uploadTaskFn()` | FreeRTOS task for background encode/upload (core 0) |
| `startRecording()` / `recordAudioChunk()` | Mic DMA capture |
| `updateDisplay()` | Screen dispatcher |
| `handleButtons()` | Central button/touch dispatcher |

## Configuration

All compile-time defaults are in `VConRecorder/config.h`. Runtime overrides are persisted to NVS flash via the `Preferences` library and take precedence.

Key settings: WiFi SSID/password, POST URL, device token, recording duration (10-120s), sample rate (8 kHz), firmware version.

Serial commands at 115200 baud: `wifi`, `url`, `token`, `dur`, `status`, `scan`, `sd`, `restart`, `help`.

## Testing

`test_device.sh` runs 17 integration tests over a serial connection using pyserial. It requires a physical device connected via USB:

```bash
./test_device.sh /dev/cu.usbserial-XXXXX
```

There are no unit tests or CI pipelines — testing is device-based.

## OTA Updates

The device checks for firmware updates on every boot after WiFi connects. The OTA server is at `vcon-gateway.replit.app/api/ota/`. Deploy order matters: upload `firmware.bin` before updating `version.txt`.

## Code Conventions

- **Language:** C++ (Arduino dialect), single `.ino` file with `.h` headers
- **Naming:** `camelCase` for functions and local variables, `UPPER_SNAKE` for `#define` constants, `g_` prefix for global task handles
- **State:** enum-based state machine (`AppState`, `UIScreen`, `ToolsPhase`)
- **Concurrency:** `volatile` for cross-core shared primitives, no mutexes
- **Memory:** PSRAM for large buffers (`ps_malloc`), heap for small allocations
- **Display:** M5Unified `M5.Display` API with an off-screen `M5Canvas` sprite for flicker-free VU meter
- **Color:** VConic brand green `#30CC30` → RGB565 `0x3666` (`VC_GREEN`)
- **Serial logging:** prefixed tags like `[wifi]`, `[sd]`, `[OTA]`, `[ntp]`
- **Error handling:** retry with exponential backoff for HTTP POST (3 retries: 5s → 30s → 5min)

## Important Constraints

- **PSRAM budget is tight.** Any buffer changes must account for the memory math in `config.h`. Continuous mode peaks at ~2.9 MB for 45s chunks with only ~800 KB margin.
- **Partition scheme must be `min_spiffs`** for OTA to work. Default partition scheme will cause `Update.begin()` to fail.
- **WiFi SSID parsing:** the `wifi` serial command treats the last word as the password; everything before it is the SSID (supports spaces in SSIDs).
- **`FIRMWARE_VERSION` in `config.h` must match the OTA server's `version.txt`** for the running release, or devices will re-flash on every boot.
- **The `.gitignore` keeps `firmware.bin` in the repo** but excludes other build artifacts.
