# M5Stack Core2 — VConic vCon Audio Recorder

Records audio from the M5Stack Core2's built-in microphone and publishes each
chunk as a spec-compliant [vCon](https://datatracker.ietf.org/doc/draft-ietf-vcon-vcon-core/)
JSON document (IETF draft-ietf-vcon-vcon-core v0.4.0) to the
[VConic gateway](https://vcon-gateway.replit.app). Recordings are configurable
from 10–120 seconds, run continuously with zero-gap dual-buffer mode (≤ 45 s),
and are simultaneously saved to a microSD card organized into date/hour
directories with automatic rotation.

---

## Table of Contents

1. [Hardware Requirements](#1-hardware-requirements)
2. [Quick Start](#2-quick-start)
3. [User Interface](#3-user-interface)
4. [Serial Command Reference](#4-serial-command-reference)
5. [Configuration Reference](#5-configuration-reference)
6. [vCon Output Format](#6-vcon-output-format)
7. [SD Card Storage](#7-sd-card-storage)
8. [Memory Architecture](#8-memory-architecture)
9. [Build & Flash](#9-build--flash)
10. [OTA Firmware Updates](#10-ota-firmware-updates)
11. [Connecting via USB from macOS Terminal](#11-connecting-via-usb-from-macos-terminal)
12. [Troubleshooting](#12-troubleshooting)
13. [Project Structure](#13-project-structure)
14. [Dependencies](#14-dependencies)

---

## 1. Hardware Requirements

| Component | Detail |
|-----------|--------|
| **Board** | M5Stack Core2 |
| **MCU** | ESP32-D0WDQ6-V3 (dual-core 240 MHz) |
| **PSRAM** | 8 MB external (≈ 4 MB usable after system overhead) |
| **Display** | 320 × 240 IPS LCD (capacitive touch) |
| **Microphone** | Built-in PDM MEMS microphone |
| **Storage** | microSD card (FAT32, any capacity) — optional |
| **Connectivity** | 2.4 GHz 802.11 b/g/n Wi-Fi |
| **Power** | USB-C or internal 390 mAh LiPo |

> **microSD card:** Must be formatted as **FAT32** before first use.
> The device boots normally without a card; SD features are simply skipped.

---

## 2. Quick Start

1. **Flash** the firmware (see [§9 Build & Flash](#9-build--flash)).
2. **Connect** a serial terminal at **115200 baud**.
3. **Configure WiFi** via the serial console:
   ```
   wifi MyNetwork mypassword
   ```
   For SSIDs with spaces (the last word is always taken as the password):
   ```
   wifi Barnhill Tavern meteormeteor
   ```
4. **Configure a portal token** (optional — enables account-level routing):
   ```
   token dvt_your_token_here
   ```
   Without a token, recordings are submitted as unassigned and can be claimed
   in the VConic portal by MAC address.
5. **Insert** a FAT32-formatted microSD card (optional).
6. On the **HOME screen**, press **Button A (RUN)** to begin continuous
   recording and posting.
7. Press **Button B (STOP)** to finish the current chunk and return to idle.

---

## 3. User Interface

The display uses a four-screen navigation system. Each screen occupies the
full 320 × 240 display with a shared 8 px VConic green (#30CC30) hazard strip
at the top and a 46 px button row at the bottom.

### HOME Screen (default)

The main at-a-glance view shown on boot and after any inactivity timeout
(60 seconds).

```
┌─────────────────────────────────────────────────────────────────┐
│ ▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓ HAZARD STRIP ▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓ │ y=0
├─────────────────────────────────────────────────────────────────┤ y=8
│  WiFi: CONNECTED  192.168.1.42  -44 dBm   14:22:35 UTC         │
│  SSID: barnhill tavern          NTP: yes  2026-04-01            │
├─────────────────────────────────────────────────────────────────┤
│                                                                  │
│                          IDLE                                    │
│                     (large state text)                           │
│                                                                  │
├─────────────────────────────────────────────────────────────────┤
│  OK: 148    Err: 8    Dur: 60s    Upload: —                     │
├─────────────────────────────────────────────────────────────────┤
│  VC-832924  84:1F:E8:83:29:24                                   │
├─────────────────────────────────────────────────────────────────┤ y=194
│    [ RUN ]              [ CONFIG ]           [ TOOLS ]          │
└─────────────────────────────────────────────────────────────────┘ y=240
```

**Button labels change with state:**

| App State | Button A | Button B | Button C |
|-----------|----------|----------|----------|
| IDLE | **RUN** | **CONFIG** | **TOOLS** |
| RECORDING | _(dim)_ | **STOP** | **STATUS** |
| PROCESSING | _(dim)_ | _(dim)_ | **STATUS** |

---

### STATUS Screen

Real-time recording dashboard — accessible via Button C while recording,
or by navigating from any screen.

```
┌─────────────────────────────────────────────────────────────────┐
│ ▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓ HAZARD STRIP ▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓ │
├─────────────────────────────────────────────────────────────────┤
│  MICROPHONE INPUT                                               │
│  ┌──┐ ┌────┐ ┌──┐ ┌───┐ ┌──┐ ┌────┐ ┌──┐ ┌────┐ ┌──┐         │
│  │  │ │    │ │  │ │   │ │  │ │    │ │  │ │    │ │  │         │
│  └──┘ └────┘ └──┘ └───┘ └──┘ └────┘ └──┘ └────┘ └──┘         │
├─────────────────────────────────────────────────────────────────┤
│  Elapsed: 00:32 / 01:00   ████████████████░░░░░░░░  53%        │
├─────────────────────────────────────────────────────────────────┤
│  Upload: ENCODING                                               │
├─────────────────────────────────────────────────────────────────┤
│  UUID: a54553f6f1...                                            │
├─────────────────────────────────────────────────────────────────┤
│  OK: 148    Fail: 8                                             │
├─────────────────────────────────────────────────────────────────┤
│  Free PSRAM: 3187 KB                                            │
├─────────────────────────────────────────────────────────────────┤
│  Mode: single-shot    SD: 3 saved                               │
├─────────────────────────────────────────────────────────────────┤
│    [ HOME ]              [ — ]                [ — ]            │
└─────────────────────────────────────────────────────────────────┘
```

| Region | Content |
|--------|---------|
| VU meter | 16-bar rolling level display (green / yellow / red) |
| Progress | Elapsed MM:SS / total, filled progress bar, percentage |
| Upload | Current upload-task state: IDLE, ENCODING, SD, POST, OK, FAIL |
| UUID | Last 10 chars of most-recent vCon UUID |
| Counters | Total OK posts and failures this session |
| Buffer | Free PSRAM in KB |
| Mode | `continuous` or `single-shot`, plus SD save count |

---

### CONFIG Screen

Read-only view of all current settings — no interaction needed, just a
reference you can check without opening a serial terminal.

```
┌─────────────────────────────────────────────────────────────────┐
│ ▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓ HAZARD STRIP ▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓ │
│  CONFIGURATION                                                  │
│  Device ID:  VC-832924                                          │
│  MAC:        84:1F:E8:83:29:24                                  │
│  Firmware:   1.0.4                                              │
│  Token:      (none — MAC routing)                               │
│  WiFi:       barnhill tavern — CONNECTED                        │
│  IP:         192.168.86.33   RSSI: -44 dBm                      │
│  NTP:        yes  2026-04-01T14:22:35Z                          │
│  Duration:   60 s  (single-shot)                                │
│  URL: https://vcon-gateway.replit.app/ingress                   │
│  ─────────────────────────────────────────────────────          │
│  Serial commands: wifi  url  token  dur                         │
│                   status  scan  sd  restart  help               │
├─────────────────────────────────────────────────────────────────┤
│    [ HOME ]              [ — ]                [ — ]            │
└─────────────────────────────────────────────────────────────────┘
```

---

### TOOLS Screen

On-device diagnostics — run tests and check connectivity without a computer.

```
┌─────────────────────────────────────────────────────────────────┐
│ ▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓ HAZARD STRIP ▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓ │
│  TOOLS & DIAGNOSTICS                                            │
│                                                                  │
│    ▶  WiFi Test                                                 │
│       OTA Check                                                 │
│       POST Test                                                 │
│       SD Info                                                   │
│       Restart                                                   │
│                                                                  │
├─────────────────────────────────────────────────────────────────┤
│    [ HOME ]           [ SELECT ]            [ RUN ]            │
└─────────────────────────────────────────────────────────────────┘
```

**Tools:**

| Tool | What it does |
|------|-------------|
| **WiFi Test** | Reconnects to configured SSID; reports IP and RSSI |
| **OTA Check** | Fetches `version.txt` and compares to local firmware version (no flash/reboot) |
| **POST Test** | Allocates 1 s of silence, builds a vCon, POSTs it; reports HTTP response code |
| **SD Info** | Shows SD total / used / free space and session save count |
| **Restart** | Soft-resets the device (shows confirmation prompt first) |

**Navigation:**
- **Button A** → back to HOME
- **Button B** → move cursor up/down through menu items
- **Button C** → run selected tool (or confirm/cancel on the restart prompt)

Results are shown for 8 seconds then automatically return to the menu.

---

### Inactivity Timeout

Any screen other than HOME automatically returns to HOME after **60 seconds**
of no button activity.

---

## 4. Serial Command Reference

Connect at **115200 8N1** (no hardware flow control).

### `wifi <ssid> <password>`
Set WiFi credentials and immediately reconnect.

```
wifi HomeNetwork myS3cretP@ss
wifi Barnhill Tavern meteormeteor
```

> The **last word** is always treated as the password; everything before it is
> the SSID. This handles SSIDs that contain spaces.

Credentials are written to flash and survive power cycles.

---

### `url <endpoint>`
Set the vCon HTTP POST endpoint.

```
url https://vcon-gateway.replit.app/ingress
url https://api.example.com/vcons
```

Saved to flash immediately.

---

### `token <value>`
Set the VConic portal device token. When set, the token is appended as
`?token=<value>` on every POST and sent as `X-Device-Token` header, routing
recordings directly to your account (HTTP 202).

```
token dvt_abc123xyz
```

### `token`
Clear the token (no argument). Falls back to MAC-address routing. Recordings
are submitted as unassigned (HTTP 200) and can be claimed in the portal.

---

### `dur <seconds>`
Set the recording chunk length. Valid range: **10–120 seconds**.

```
dur 60     # 1 minute per chunk (default)
dur 30     # 30 seconds
dur 120    # 2 minutes (single-shot only)
```

- **10–45 s** → continuous dual-buffer mode available (recording continues
  without gaps while the previous chunk uploads in parallel on core 0).
- **46–120 s** → single-shot mode only; the device records one chunk, encodes
  and uploads it, then waits for the next RUN press. This is automatic — no
  extra configuration needed.

The new duration takes effect on the next RUN press. Buffers are reallocated
at the new size; the old buffers are freed immediately.

Setting is stored in flash and survives reboots.

Cannot be changed while recording — stop first.

### `dur`
Show the current duration without changing it.

---

### `status`
Print a full status report:

```
=== vCon Recorder Status ===
Firmware:    1.0.4
Device ID:   VC-832924
MAC Address: 84:1F:E8:83:29:24
Rec Duration:60 s (single-shot mode, range 10-120 s)
Device Token:(none — using MAC routing)
WiFi SSID:   barnhill tavern
WiFi Status: Connected
IP Address:  192.168.86.33
RSSI:        -44 dBm
NTP Synced:  Yes
POST URL:    https://vcon-gateway.replit.app/ingress
Total OK:    148
Total Fail:  8
Free Heap:   174280 bytes
Free PSRAM:  3187372 bytes
App State:   0
============================
```

---

### `scan`
Scan for nearby 2.4 GHz networks and print SSID, RSSI, and security type.

---

### `sd`
Show SD card statistics.

---

### `restart`
Soft-reset the device.

---

### `help`
Print the command list.

---

## 5. Configuration Reference

All defaults live in `VConRecorder/config.h`. Runtime overrides are stored in
NVS flash via the `Preferences` library and take precedence over compile-time
defaults.

| `#define` | Default | Description |
|-----------|---------|-------------|
| `DEFAULT_WIFI_SSID` | `"barnhill tavern"` | Initial SSID |
| `DEFAULT_WIFI_PASSWORD` | `"meteormeteor"` | Initial password |
| `DEFAULT_POST_URL` | `https://vcon-gateway.replit.app/ingress` | VConic gateway |
| `DEFAULT_DEVICE_TOKEN` | `""` | Portal token (empty = MAC routing) |
| `FIRMWARE_VERSION` | `"1.0.4"` | Current firmware version |
| `SAMPLE_RATE` | `8000` | Audio sample rate in Hz |
| `BITS_PER_SAMPLE` | `16` | PCM bit depth |
| `RECORD_DURATION_SEC` | `60` | Default chunk length in seconds |
| `MIN_RECORD_DURATION_SEC` | `10` | Shortest allowed duration |
| `MAX_RECORD_DURATION_SEC` | `120` | Longest allowed duration (PSRAM budget) |
| `CONT_MAX_DURATION_SEC` | `45` | Maximum duration for continuous dual-buffer mode |
| `OTA_VERSION_URL` | `https://vcon-gateway.replit.app/api/ota/version.txt` | OTA version check endpoint |
| `OTA_FIRMWARE_URL` | `https://vcon-gateway.replit.app/api/ota/firmware.bin` | OTA binary download endpoint |

### Buffer sizing

Buffer allocation is computed at runtime from the current `recordDurationSec`
value via inline helpers `audioSampleTarget()` and `audioPcmBytes()`. At 8 kHz /
16-bit mono:

| Duration | PCM buffer | JSON peak | Continuous peak |
|----------|-----------|-----------|-----------------|
| 30 s | 480 KB | ~1.6 MB | ~2.1 MB ✓ |
| 45 s | 720 KB | ~2.4 MB | ~2.7 MB ✓ (~800 KB margin) |
| 60 s | 960 KB | ~3.2 MB | ✗ single-shot only (49 KB over limit) |
| 90 s | 1.4 MB | ~4.8 MB | ✗ single-shot only |
| 120 s | 1.9 MB | ~6.4 MB | ✗ single-shot only |

---

## 6. vCon Output Format

The device produces JSON compliant with **draft-ietf-vcon-vcon-core v0.4.0**.

### Top-level fields

| Field | Value |
|-------|-------|
| `vcon` | `"0.4.0"` — spec version |
| `uuid` | UUID v8 (boot timer + `esp_random()`) |
| `created_at` | ISO 8601 UTC timestamp (NTP-synced) |
| `extensions` | `["meta"]` — declares the `meta` field used in `parties` |

### `parties` array

```json
"parties": [{
  "name": "M5Stack Recorder",
  "role": "recorder",
  "meta": {
    "device_id": "84:1f:e8:83:29:24",
    "vconic_id": "VC-832924",
    "device_type": "m5stack-core2"
  }
}]
```

`device_id` is the full colon-format MAC address — the VConic gateway uses
this field for MAC-based device routing when no token is provided.

### `dialog` array

```json
"dialog": [{
  "type": "recording",
  "start": "2026-04-01T14:22:00Z",
  "duration": 60,
  "parties": [0],
  "originator": 0,
  "mimetype": "audio/wav",
  "encoding": "base64url",
  "body": "<base64url-encoded RIFF/WAV>"
}]
```

`duration` reflects the actual captured seconds (`audioBufferIndex / SAMPLE_RATE`),
which may be less than the configured value when STOP is pressed mid-chunk.

### `attachments` array

```json
"attachments": [{
  "purpose": "tags",
  "start": "2026-04-01T14:22:00Z",
  "party": 0,
  "dialog": 0,
  "encoding": "json",
  "body": "{\"source\":\"m5stack-core2\",\"device_id\":\"VC-832924\",\"mac\":\"84:1f:e8:83:29:24\",\"sample_rate\":8000,\"duration_seconds\":60}"
}]
```

### HTTP POST details

| Header | Value |
|--------|-------|
| `Content-Type` | `application/json` |
| `X-Device-Token` | Token value (only when token is set) |

**URL:** `<POST_URL>?token=<token>` (token routing) or just `<POST_URL>` (MAC routing)

**Gateway responses:**

| Code | Meaning |
|------|---------|
| `202` | Routed to account — recording linked to your portal device |
| `200` | Accepted as unassigned — claim it in the portal by MAC |
| `400` | Bad payload — check serial output for encoding errors |
| `5xx` | Server error — device retries 3× (5 s → 30 s → 5 min back-off) |

---

## 7. SD Card Storage

### Setup

1. Format the card as **FAT32** on a computer.
2. Insert into the microSD slot on the side of the Core2.
3. Power-cycle or `restart` — `[sd]` log lines confirm the card size.

### Directory layout

Files are organized by date and hour for easy navigation:

```
/
├── wav/
│   └── YYYY/
│       └── MM/
│           └── DD/
│               └── HH/
│                   ├── <uuid>.wav    ← RIFF/WAV, 16-bit mono, 8 kHz
│                   └── ...
└── vcons/
    └── YYYY/
        └── MM/
            └── DD/
                └── HH/
                    ├── <uuid>.json   ← Complete vCon JSON
                    └── ...
```

### Automatic rotation

Before each save, the device checks free space. If less than **20 MB**
remains, the oldest hour-directory under `/wav` and `/vcons` is deleted until
enough space is available. This runs silently and logs to serial:

```
[sd] low space (4 MB free) — pruning old files
[sd] pruning: /wav/2026/03/31/14
```

### Save order

1. **WAV saved to SD** — written directly from PSRAM before any encoding
   allocations, so the audio is never lost to a PSRAM failure.
2. **JSON saved to SD** — written from the completed JSON buffer.
3. **HTTP POST** — uploaded to the configured endpoint.

A recording is always on the card before the POST is attempted.

---

## 8. Memory Architecture

### PSRAM budget

#### Normal (single-shot) mode — 60 s example

| Region | Size | Lifetime |
|--------|------|----------|
| Audio PCM buffer A | 960 000 B (≈ 938 KB) | Persistent |
| vCon JSON buffer | ≈ 1 280 000 B (≈ 1 250 KB) | During encode/upload |
| WAV staging buffer | 960 044 B (≈ 938 KB) | Briefly during encode |
| **Peak total** | **≈ 3.1 MB** | Within ≈ 4 MB available |

#### Continuous mode (dual-buffer) — 45 s example

| Region | Size | Lifetime |
|--------|------|----------|
| Audio buffer A | 720 000 B (≈ 703 KB) | Persistent |
| Audio buffer B | 720 000 B (≈ 703 KB) | Persistent |
| vCon JSON buffer | ≈ 960 000 B (≈ 938 KB) | During encode on core 0 |
| WAV staging buffer | 720 044 B (≈ 703 KB) | Briefly during encode |
| **Peak total** | **≈ 2.9 MB** | ~800 KB below ≈ 3.7 MB available |

> At 60 s, continuous mode would need ~4.16 MB but only ~4.02 MB is available
> after WiFi/IDF overhead — a 49 KB shortfall. The firmware automatically uses
> single-shot mode for durations above `CONT_MAX_DURATION_SEC` (45 s).

### Dual-buffer continuous recording

In continuous mode the ESP32's two cores work independently:

- **Core 1 (main loop):** mic DMA recording, display updates, button events
- **Core 0 (upload task):** base64 encoding, SD write, HTTP POST

When a chunk completes:
1. Core 1 swaps the recording pointer to the alternate buffer and immediately
   continues filling it — **zero recording gap**.
2. The completed buffer is handed to a FreeRTOS task on core 0 for
   encoding and upload.
3. Both cores run simultaneously for the next chunk.

If the upload takes longer than the chunk duration (e.g. very slow WiFi), the
recording buffer stretches until the task finishes, then swaps cleanly.

### Single-buffer encoding strategy

Within each upload:
1. Allocate one `jsonBuf` sized for `prefix + base64 + suffix`.
2. Encode WAV directly into `jsonBuf[prefixLen]` via `mbedtls_base64_encode`.
3. Free the WAV staging buffer immediately after encoding.
4. Convert standard base64 → base64url **in-place** (no second buffer).

---

## 9. Build & Flash

> **Using Claude Code?** The repo includes a `flash-controller` skill that handles the full workflow automatically — port discovery, version check, compile (on request), flash, and boot verification. Just say *"flash the device"* or *"update the M5"*.

### Prerequisites

- **Arduino IDE 2.x** (includes arduino-cli at a known path on macOS)
- ESP32 Arduino core (`esp32:esp32`) v3.x installed
- **M5Unified** library v0.2.13 or later

On macOS, arduino-cli is bundled inside the Arduino IDE app:
```
/Applications/Arduino IDE.app/Contents/Resources/app/lib/backend/resources/arduino-cli
```

Or install standalone via Homebrew:
```bash
brew install arduino-cli
```

### Compile

The sketch requires the **Minimal SPIFFS (1.9 MB APP with OTA)** partition
scheme so the ESP32 has two equal-sized OTA partitions for over-the-air
updates. Pass it via build properties:

```bash
arduino-cli compile \
  --fqbn esp32:esp32:m5stack_core2 \
  --build-property "build.partitions=min_spiffs" \
  --build-property "upload.maximum_size=1966080" \
  --output-dir VConRecorder/build \
  VConRecorder/VConRecorder.ino
```

A successful build reports roughly:
```
Sketch uses 1418051 bytes (72%) of program storage space.  Maximum is 1966080 bytes.
Global variables use 54588 bytes (1%) of dynamic memory.
```

> **Why 72%?** The `min_spiffs` scheme allocates two 1.9 MB OTA app partitions.
> The sketch fits with ~550 KB to spare — enough headroom for future growth.

### Flash

```bash
arduino-cli upload \
  --fqbn esp32:esp32:m5stack_core2 \
  --port /dev/cu.usbserial-5B212355431 \
  --input-dir VConRecorder/build \
  VConRecorder/VConRecorder.ino
```

Replace the port with your device's actual serial port
(`/dev/ttyUSB0` on Linux, `COMx` on Windows).

If the port is held by a monitor process:
```bash
pkill -f "arduino-cli monitor"
pkill -f "screen.*usbserial"
```

---

## 10. OTA Firmware Updates

The device checks for a firmware update on every boot, immediately after
WiFi connects. If a newer version is available it downloads and flashes the
binary in place, then reboots into the new firmware automatically — no serial
cable or computer required.

### How it works

```
boot → WiFi connected
         │
         ▼
   GET /api/ota/version.txt   ← plain-text remote version string  (e.g. "1.0.4")
         │
   compare to FIRMWARE_VERSION baked into this build
         │
   equal? ──── YES ──▶  skip, continue normal startup
         │
         NO
         ▼
   GET /api/ota/firmware.bin  ← raw ESP32 .bin, Content-Length required
         │
   stream into the inactive OTA partition via ESP32 Update library
         │
   reboot into new firmware
```

### Version contract

| Scenario | Behaviour |
|----------|-----------|
| Remote version == local | Skip update, log `[OTA] firmware is current` |
| Remote version != local | Download and flash, then reboot |
| `/api/ota/version.txt` returns non-200 | Log error, continue boot normally |
| `/api/ota/firmware.bin` missing Content-Length | Abort, continue boot normally |
| Flash write error | Log error, continue boot normally (old firmware intact) |

### Serial output

```
[OTA] local=1.0.3  remote=1.0.4
[OTA] update available (1.0.3 → 1.0.4), downloading...
[OTA] binary size: 1418192 bytes
[OTA]  141819 / 1418192 bytes (10%)
[OTA]  283638 / 1418192 bytes (20%)
...
[OTA] 1418192 / 1418192 bytes (100%)
[OTA] success — rebooting in 1 s...
```

### Shipping a firmware update

#### Step 1 — Compile and note the binary path

```bash
arduino-cli compile \
  --fqbn esp32:esp32:m5stack_core2 \
  --build-property "build.partitions=min_spiffs" \
  --build-property "upload.maximum_size=1966080" \
  --output-dir VConRecorder/build \
  VConRecorder/VConRecorder.ino
```

Binary lands at:
```
VConRecorder/build/esp32.esp32.m5stack_core2/VConRecorder.ino.bin
```

#### Step 2 — Get an auth token

```bash
TOKEN=$(curl -s -X POST https://vcon-gateway.replit.app/api/auth/login \
  -H "Content-Type: application/json" \
  -d '{"email":"you@example.com","password":"yourpassword"}' \
  | python3 -c "import sys,json; print(json.load(sys.stdin)['token'])")
```

#### Step 3 — Upload the firmware binary

```bash
python3 - <<PYEOF
import base64, json, urllib.request

with open("VConRecorder/build/esp32.esp32.m5stack_core2/VConRecorder.ino.bin", "rb") as f:
    data = base64.b64encode(f.read()).decode("ascii")

payload = json.dumps({"firmwareBase64": data}).encode("utf-8")
req = urllib.request.Request(
    "https://vcon-gateway.replit.app/api/ota/firmware",
    data=payload,
    headers={"Authorization": f"Bearer $TOKEN", "Content-Type": "application/json"},
    method="POST"
)
with urllib.request.urlopen(req, timeout=120) as resp:
    print(resp.read().decode())
PYEOF
```

> **macOS note:** `base64 -w0` is a Linux flag. Use Python (shown above) or
> `base64 -b 0 -i firmware.bin` for reliable cross-platform encoding of
> large binaries.

#### Step 4 — Set the version string

```bash
curl -X PUT https://vcon-gateway.replit.app/api/ota/version \
  -H "Authorization: Bearer $TOKEN" \
  -H "Content-Type: application/json" \
  -d '{"version":"1.0.4"}'
```

#### Step 5 — Verify (no auth required)

```bash
curl https://vcon-gateway.replit.app/api/ota/version.txt
# → 1.0.4

curl -I https://vcon-gateway.replit.app/api/ota/firmware.bin
# → content-length: 1418192
```

> ⚠️ **Deploy order matters.** Always upload `firmware.bin` *before* updating
> `version.txt`. Devices check the version string first — if `version.txt`
> already shows the new version but `firmware.bin` is still the old build,
> devices will try to download a mismatched binary.

### Partition scheme requirement

OTA requires two equal-sized app partitions on the ESP32. The sketch must be
compiled with `build.partitions=min_spiffs` (see [§9 Build & Flash](#9-build--flash)).
Building with the default partition scheme disables OTA and the
`Update.begin()` call will fail at runtime.

---

## 11. Connecting via USB from macOS Terminal

No extra software is required — macOS ships with the `screen` utility.

### Step 1 — Find the port

```bash
ls /dev/cu.*
```

Look for a name containing `usbserial`, e.g.:
```
/dev/cu.usbserial-5B212355431
```

### Step 2 — Open a terminal session

```bash
screen /dev/cu.usbserial-5B212355431 115200,crnl
```

`crnl` enables correct CR/LF translation so lines wrap properly.

**To exit:** press `Ctrl-A`, then `K`, then `Y`.

**To enable local echo** (so you can see what you type):
Press `Ctrl-A E` after opening the session.

---

### Alternatives

**`arduino-cli monitor`:**
```bash
arduino-cli monitor \
  --port /dev/cu.usbserial-5B212355431 \
  --config baudrate=115200
```
Exit with `Ctrl-C`. Must be killed before flashing.

**Python** (scripted / non-interactive):
```bash
# One-time setup
python3 -m venv /tmp/venv && /tmp/venv/bin/pip install pyserial -q

# Send a command and read response
/tmp/venv/bin/python3 - <<'EOF'
import serial, time
s = serial.Serial('/dev/cu.usbserial-5B212355431', 115200, timeout=3)
time.sleep(0.5)
s.write(b'status\r\n')
time.sleep(2)
print(s.read(s.in_waiting or 4096).decode(errors='replace'))
s.close()
EOF
```

### Tip — see the full boot log

Open the terminal session **before** pressing reset to see every `[init]`,
`[wifi]`, `[sd]`, `[ntp]`, and `[OTA]` log line from startup.

---

## 12. Troubleshooting

### WiFi will not connect / "sta is connecting, cannot set config"

The firmware fully resets the WiFi stack before each attempt. If it persists,
send `restart` via serial.

---

### SSID with spaces is parsed incorrectly

The `wifi` command splits on the **last** space — everything before the last
word is the SSID:

```
wifi My Network With Spaces thepassword
          ↑ SSID                ↑ password
```

---

### SD card not detected

1. Format as **FAT32** (not exFAT or NTFS).
2. Re-seat the card and send `restart`.
3. Check serial for `[sd]` log lines at boot.

---

### NTP not synced / timestamps show 1970-01-01

WiFi must be connected before NTP can sync. Connect first, then `restart`.
Timestamps fall back to a millis()-derived relative value when unavailable.

---

### Upload returns HTTP 400

The vCon JSON payload has a structural problem. Check serial output for
encoding errors and verify the POST URL accepts `application/json`.

---

### Upload returns HTTP 200 instead of 202

The device has no portal token set, so the recording was accepted as
unassigned. Either:
- Claim it in the VConic portal by MAC address, or
- Set a token: `token dvt_your_token_here`

---

### Continuous mode: recording pauses briefly

If an upload takes longer than the chunk duration (very slow WiFi), the mic
pauses while the previous chunk finishes uploading. Recording resumes
automatically. Check RSSI in the home screen — signal below -75 dBm causes
slow uploads.

---

### OTA check returns HTTP 404 on boot

```
[OTA] version check failed: HTTP 404
```

The firmware binary or version string has not been uploaded to the OTA server
yet. See [§10 Shipping a firmware update](#shipping-a-firmware-update) for the
three-step upload process.

---

### Display stays on "Initializing..." / PSRAM error screen

Board FQBN must be `esp32:esp32:m5stack_core2`. Confirm you are not building
for a generic ESP32 or M5Stack Core (non-Core2).

---

## 13. Project Structure

```
vcon-m5-stack-audio/
├── README.md                         ← This file
├── test_device.sh                    ← 17 automated device tests (pyserial)
└── VConRecorder/
    ├── VConRecorder.ino              ← Main Arduino sketch
    ├── config.h                      ← Compile-time defaults, buffer constants & OTA URLs
    ├── ota.h                         ← OTA update logic (HTTP poll + ESP32 Update library)
    └── logo.h                        ← VConic logo (RGB565, 310×110 px, PROGMEM)
```

### Key functions in VConRecorder.ino

| Function | Description |
|----------|-------------|
| `loadConfig()` / `saveConfig()` | Read/write NVS flash (WiFi, URL, token, counters) |
| `connectWiFi()` | Full-reset WiFi stack, attempt connection |
| `initSD()` | Mount SD card via explicit `SPI.begin(18,38,23,4)` |
| `buildDateDir(buf, size, root)` | Build `/root/YYYY/MM/DD/HH` path from current time |
| `mkdirP(path)` | Create directory tree (mkdir -p) |
| `ensureSDSpace()` | Prune oldest hour-dirs if free space < 20 MB |
| `saveWavToSD(buf, n, uuid)` | Stream WAV from PSRAM buffer to SD |
| `saveVConToSD(uuid, json, len)` | Write vCon JSON to SD |
| `buildAndUploadVConCore(buf, n)` | Encode + SD save + HTTP POST (no display calls) |
| `buildAndUploadVCon()` | Single-shot wrapper; calls core with globals |
| `uploadTaskFn(param)` | FreeRTOS task (core 0): encode/upload for continuous mode |
| `startUploadTask(buf, n)` | Launch upload task with given buffer |
| `swapAndContinue()` | Swap dual buffers + launch upload task each chunk |
| `allocateAudioBuffer()` | Allocate PSRAM buffer A at startup |
| `allocateContinuousBuffers()` | Allocate PSRAM buffer B on first RUN press |
| `startRecording()` | Initialise mic, set STATE_RECORDING or STATE_CONTINUOUS |
| `recordAudioChunk()` | Capture 1 024 samples each loop() iteration |
| `updateDisplay()` | Screen dispatcher → one of four draw functions |
| `drawHomeScreen()` | Connection/state/stats summary (default view) |
| `drawStatusScreen()` | Real-time VU meter, progress, upload state |
| `drawConfigScreen()` | Read-only settings reference |
| `drawToolsScreen()` | 4-phase diagnostic tool menu |
| `handleButtons()` | Central button dispatcher (delegates to per-screen handlers) |
| `handleToolsButtons()` | TOOLS-screen button navigation and tool execution |
| `toolsRunWifiTest()` | Reconnect WiFi, report IP + RSSI |
| `toolsRunOtaTest()` | Fetch version.txt, compare to local (no flash/reboot) |
| `toolsRunPostTest()` | POST 1 s silence vCon, report HTTP response code |
| `toolsShowSD()` | Report SD total/used/free/saved |
| `showLogoSplash()` | 10-second boot splash with device ID |
| `handleSerialCommand(cmd)` | Parse and execute serial commands |

### Automated test script

`test_device.sh` runs 17 tests against a live device over serial:

```bash
bash test_device.sh /dev/cu.usbserial-5B212355431
```

Tests cover: `status` field presence, firmware version format, WiFi
connectivity, NTP sync, `dur` command, `url` command, `help` completeness,
unknown-command handling, OTA endpoint HTTP 200 + Content-Length, device
restart detection, post-reboot firmware version, and WiFi reconnect after
restart.

---

## 14. Dependencies

| Library | Version | Source |
|---------|---------|--------|
| **M5Unified** | ≥ 0.2.13 | `arduino-cli lib install "M5Unified"` |
| **WiFi** | bundled with ESP32 core | — |
| **HTTPClient** | bundled with ESP32 core | — |
| **Preferences** | bundled with ESP32 core | — |
| **SD** | bundled with ESP32 core | — |
| **mbedtls/base64.h** | bundled with ESP32 core (IDF) | — |
| **esp_system.h / esp_mac.h** | bundled with ESP32 core (IDF) | — |

All dependencies ship with the standard `esp32:esp32` Arduino core ≥ 3.x,
except M5Unified which must be installed separately.

---

*Updated 2026-04-01 — firmware 1.0.4: four-screen UI, on-device diagnostics, programmable duration, dual-buffer continuous recording, OTA updates*
