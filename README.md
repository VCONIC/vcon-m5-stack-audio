# M5Stack Core2 — VConic vCon Audio Recorder

Continuously records audio from the M5Stack Core2's built-in microphone and
publishes each 15-second chunk as a spec-compliant
[vCon](https://datatracker.ietf.org/doc/draft-ietf-vcon-vcon-core/) JSON
document (IETF draft-ietf-vcon-vcon-core v0.4.0) to the
[VConic gateway](https://vcon-gateway.replit.app). Every recording is
simultaneously saved to a microSD card as a raw WAV file and the complete
vCon JSON, organized into date/hour directories with automatic rotation.

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
6. **Press Button A (RUN)** to begin continuous recording and posting.
7. **Press Button B (STOP)** to finish the current chunk and go idle.

---

## 3. User Interface

### Splash Screen

On boot the device shows the VConic logo with the device's unique identifier
(`VC-XXXXXX`) centered below it for 10 seconds.

### Main Display

The display is divided into a factory-instrument-style panel layout using
VConic green (#30CC30) for all panel chrome.

```
┌──────────────────────────────────────────────────────────────────┐
│ ▓▓▓▓▓▓▓▓▓▓▓▓▓▓ HAZARD STRIP ▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓ │ y=0
├──────────┬────────────────────────────┬───────────────────────────┤ y=8
│  WIFI    │   M5STACK VCON RECORDER    │   STATE                   │
│ CONNECTED│       HH:MM:SS             │   IDLE                    │
│ -42 dBm  │       YYYY-MM-DD           │   UP: 00:03:21            │
│ barnhill │                            │   2048KB                  │
├──────────┼────────────────────────────┼───────────────────────────┤ y=68
│  RECORD  │  MICROPHONE INPUT          │   vCON                    │
│  Press   │  ┌─┐ ┌──┐ ┌─┐ ┌──┐ ┌──┐  │   UUID:                   │
│  RUN     │  │ │ │  │ │ │ │  │ │  │  │   a54553f6f1               │
│  to rec  │  └─┘ └──┘ └─┘ └──┘ └──┘  │   HTTP: 202               │
│  OK:  3  │   (VU meter bars)          │   240.0KB                 │
│  Err: 0  │                            │   POST #3                 │
│  Buf:  0 │                            │   sd: 3                   │
├──────────┴────────────────────────────┴───────────────────────────┤ y=156
│  STATUS: Continuous: chunk 4...                                    │
│  https://vcon-gateway.replit.app/ingress                          │
├──────────────────────────────────────────────────────────────────┤ y=194
│    [ RUN ]              [ STOP ]            [ CONFIG ]            │
└──────────────────────────────────────────────────────────────────┘ y=240
```

### Panel Descriptions

#### WIFI (top-left, 80 × 60 px)
| State | Content |
|-------|---------|
| Connected | Green label, RSSI in dBm, SSID (truncated), NTP status |
| Disconnected | Red label, instructions to use the `wifi` serial command |

#### TIME (top-center, 160 × 60 px)
- Large HH:MM:SS clock (NTP-synced UTC)
- YYYY-MM-DD date below

#### STATE (top-right, 80 × 60 px)
| Colour | State |
|--------|-------|
| Green / black | IDLE |
| Red / white | RECORDING or LIVE (continuous) |
| Green / black | ENCODING or UPLOADING |
| Green / black | SUCCESS |
| Red / white | ERROR |

Also shows device uptime and free PSRAM.

#### RECORD (middle-left, 80 × 87 px)
- **Idle:** "Press RUN to record", upload OK/error counts, buffer size
- **Recording:** elapsed/total time (`MM:SS/00:15`), red progress bar, percentage
- **Continuous:** elapsed/total time, green progress bar, chunk counter (`#N`)

#### MICROPHONE INPUT (middle-center, 160 × 87 px)
16-bar rolling VU meter:
- **Green** — normal level (< 40 % of peak)
- **Yellow** — moderate level (40–70 %)
- **Red** — high level (> 70 %)
- **Flat line** — idle (not recording)

#### vCON (middle-right, 80 × 87 px)
| Row | Description |
|-----|-------------|
| UUID | Last 10 chars of most recent vCon UUID |
| HTTP | Response code from last POST (202 = routed, 200 = unassigned) |
| Buffer KB | Size of captured PCM audio |
| Upload state | In continuous mode: `ENC`, `SD`, `POST`, `OK`, `FAIL`, or `RTRY #N` |
| sd: N | Recordings saved to SD this session |

#### STATUS ROW (y = 156–193)
- **Continuous/Recording:** full-width progress bar + status message
- **Idle:** human-readable status string and current POST URL

#### BUTTON ROW (y = 194–239)

| Button | Label | Function |
|--------|-------|----------|
| A (left) | **RUN** | Start continuous record+post loop |
| B (center) | **STOP** | Finish current chunk then stop; or abort single-shot early |
| C (right) | **CONFIG** | Open the configuration screen |

RUN is grey while recording/processing. STOP is red only while active.

### Config Screen (Button C)
A full-screen modal showing:
- Device ID (`VC-XXXXXX`) and MAC address
- Portal token (or "(none)" if using MAC routing)
- WiFi SSID, status, IP, RSSI
- NTP sync status and current UTC timestamp
- SD card status and session save count
- POST URL (word-wrapped)
- Available serial commands as a quick reference

Press **any button** to dismiss.

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

- **10–60 s** → continuous dual-buffer mode available (recording continues
  without gaps while the previous chunk uploads in parallel on core 0).
- **61–120 s** → single-shot mode only; the device records one chunk, encodes
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
Device ID:   VC-832924
MAC Address: 84:1f:e8:83:29:24
Firmware:    1.0.0
Rec Duration:60 s (continuous mode, range 10-120 s)
Device Token: dvt_abc123  (or "(none — using MAC routing)")
WiFi SSID:   barnhill tavern
WiFi Status: Connected
IP Address:  192.168.1.42
RSSI:        -42 dBm
NTP Synced:  Yes
POST URL:    https://vcon-gateway.replit.app/ingress
Total OK:    12
Total Fail:  0
Free Heap:   201372 bytes
Free PSRAM:  2138260 bytes
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
| `SAMPLE_RATE` | `8000` | Audio sample rate in Hz |
| `BITS_PER_SAMPLE` | `16` | PCM bit depth |
| `RECORD_DURATION_SEC` | `60` | Default chunk length in seconds |
| `MIN_RECORD_DURATION_SEC` | `10` | Shortest allowed duration |
| `MAX_RECORD_DURATION_SEC` | `120` | Longest allowed duration (PSRAM budget) |
| `CONT_MAX_DURATION_SEC` | `60` | Maximum duration for continuous mode |
| `SD_MIN_FREE_BYTES` | `20 MB` | Minimum free SD space before rotation |

### Buffer sizing

Buffer allocation is computed at runtime from the current `recordDurationSec`
value via inline helpers `audioSampleTarget()` and `audioPcmBytes()`. At 8 kHz /
16-bit mono:

| Duration | PCM buffer | JSON peak | Continuous peak |
|----------|-----------|-----------|-----------------|
| 30 s | 480 KB | ~1.6 MB | ~2.1 MB ✓ |
| 60 s | 960 KB | ~3.2 MB | ~4.2 MB ✓ |
| 90 s | 1.4 MB | ~4.8 MB | ~6.2 MB ✗ (single-shot only) |
| 120 s | 1.9 MB | ~6.4 MB | ✗ (single-shot only) |

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
  "duration": 15,
  "parties": [0],
  "originator": 0,
  "mimetype": "audio/wav",
  "encoding": "base64url",
  "body": "<base64url-encoded RIFF/WAV>"
}]
```

`duration` reflects the actual captured seconds (`audioBufferIndex / SAMPLE_RATE`),
which may be less than 15 when STOP is pressed mid-chunk.

### `attachments` array

```json
"attachments": [{
  "purpose": "tags",
  "start": "2026-04-01T14:22:00Z",
  "party": 0,
  "dialog": 0,
  "encoding": "json",
  "body": "{\"source\":\"m5stack-core2\",\"device_id\":\"VC-832924\",\"mac\":\"84:1f:e8:83:29:24\",\"sample_rate\":8000,\"duration_seconds\":15}"
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

#### Normal (single-shot) mode

| Region | Size | Lifetime |
|--------|------|----------|
| Audio PCM buffer A | 240 000 B (≈ 234 KB) | Persistent |
| vCon JSON buffer | ≈ 321 000 B (≈ 314 KB) | During encode/upload |
| WAV staging buffer | 240 044 B (≈ 235 KB) | Briefly during encode |
| **Peak total** | **≈ 795 KB** | Well within ≈ 3.4 MB available |

#### Continuous mode (dual-buffer)

| Region | Size | Lifetime |
|--------|------|----------|
| Audio buffer A | 240 000 B | Persistent — alternates as record/upload buffer |
| Audio buffer B | 240 000 B | Persistent — allocated on first RUN press |
| vCon JSON buffer | ≈ 321 000 B | During encode on core 0 |
| WAV staging buffer | 240 044 B | Briefly during encode |
| **Peak total** | **≈ 1 041 KB** | Well within available PSRAM |

### Dual-buffer continuous recording

In continuous mode the ESP32's two cores work independently:

- **Core 1 (main loop):** mic DMA recording, display updates, button events
- **Core 0 (upload task):** base64 encoding, SD write, HTTP POST

When a 15-second chunk completes:
1. Core 1 swaps the recording pointer to the alternate buffer and immediately
   continues filling it — **zero recording gap**.
2. The completed buffer is handed to a FreeRTOS task on core 0 for
   encoding and upload.
3. Both cores run simultaneously for the next 15 seconds.

If the upload takes longer than 15 seconds (e.g. very slow WiFi), the
recording buffer stretches until the task finishes, then swaps cleanly.

### Single-buffer encoding strategy

Within each upload:
1. Allocate one `jsonBuf` sized for `prefix + base64 + suffix`.
2. Encode WAV directly into `jsonBuf[prefixLen]` via `mbedtls_base64_encode`.
3. Free the WAV staging buffer immediately after encoding.
4. Convert standard base64 → base64url **in-place** (no second buffer).

---

## 9. Build & Flash

### Prerequisites

- **Arduino IDE 2.x** (includes arduino-cli at a known path on macOS)
- ESP32 Arduino core (`esp32:esp32`) v3.x installed
- **M5Unified** library v0.2.13 or later

On macOS, arduino-cli is bundled inside the Arduino IDE app:
```
/Applications/Arduino IDE.app/Contents/Resources/app/lib/backend/resources/arduino-cli
```

Create a shell alias for convenience:
```bash
alias arduino-cli="/Applications/Arduino\ IDE.app/Contents/Resources/app/lib/backend/resources/arduino-cli"
```

### Compile

The sketch requires the **Minimal SPIFFS (1.9 MB APP with OTA)** partition scheme
so the ESP32 has two equal-sized OTA partitions for over-the-air updates.
Pass it via the FQBN `PartitionScheme` option:

```bash
arduino-cli compile \
  --fqbn esp32:esp32:m5stack_core2:PartitionScheme=min_spiffs \
  VConRecorder/
```

A successful build reports roughly:
```
Sketch uses 1410907 bytes (71%) of program storage space.  Maximum is 1966080 bytes.
Global variables use 52268 bytes (1%) of dynamic memory.
```

> **Why 71%?** The `min_spiffs` scheme allocates two 1.9 MB OTA app partitions.
> The sketch fits with ~555 KB to spare — enough headroom for future growth.

### Flash

```bash
arduino-cli upload \
  --fqbn esp32:esp32:m5stack_core2 \
  --port /dev/cu.usbserial-5B212355431 \
  VConRecorder/
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
   GET /version.txt        ← plain-text remote version string  (e.g. "1.0.1")
         │
   compare to FIRMWARE_VERSION baked into this build
         │
   equal? ──── YES ──▶  skip, continue normal startup
         │
         NO
         ▼
   GET /firmware.bin       ← raw ESP32 .bin, Content-Length required
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
| `/version.txt` returns non-200 | Log error, continue boot normally |
| `/firmware.bin` missing Content-Length | Abort, continue boot normally |
| Flash write error | Log error, continue boot normally (old firmware intact) |

### Serial output

```
[OTA] local=1.0.0  remote=1.0.1
[OTA] update available (1.0.0 → 1.0.1), downloading...
[OTA] binary size: 1410907 bytes
[OTA]  141090 / 1410907 bytes (10%)
[OTA]  282180 / 1410907 bytes (20%)
...
[OTA] 1410907 / 1410907 bytes (100%)
[OTA] success — rebooting in 1 s...
```

### Shipping a firmware update

1. **Export the compiled binary** in Arduino IDE: *Sketch → Export Compiled Binary*.
   The file lands at `VConRecorder/build/esp32.esp32.m5stack_core2/VConRecorder.ino.bin`.
2. **Rename** it `firmware.bin`.
3. **Upload `firmware.bin`** to the OTA server (Replit: *Files → public → firmware.bin*).
4. **Update `version.txt`** on the server to the new version string (e.g. `1.0.1`).

> ⚠️ **Deploy order matters.** Always upload `firmware.bin` *before* updating
> `version.txt`. Devices check the version string first — if `version.txt`
> already says `1.0.1` but `firmware.bin` is still the old build, devices will
> try to download a mismatched binary.

### OTA configuration (`config.h`)

| Constant | Default | Description |
|----------|---------|-------------|
| `FIRMWARE_VERSION` | `"1.0.0"` | Version baked into this build; must match `version.txt` when this is the current release |
| `OTA_VERSION_URL` | `https://vcon-gateway.replit.app/version.txt` | Plain-text version endpoint |
| `OTA_FIRMWARE_URL` | `https://vcon-gateway.replit.app/firmware.bin` | Raw binary download endpoint |

### Partition scheme requirement

OTA requires two equal-sized app partitions on the ESP32. The sketch must be
compiled with `PartitionScheme=min_spiffs` (see [§9 Build & Flash](#9-build--flash)).
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
screen /dev/cu.usbserial-5B212355431 115200
```

**To exit:** press `Ctrl-A`, then `K`, then `Y`.

---

### Alternatives

**`arduino-cli monitor`:**
```bash
arduino-cli monitor \
  --port /dev/cu.usbserial-5B212355431 \
  --config baudrate=115200
```
Exit with `Ctrl-C`. Must be killed before flashing.

**Python** (scripted commands):
```bash
# One-time setup
python3 -m venv /tmp/venv && /tmp/venv/bin/pip install pyserial -q

# Send a command
/tmp/venv/bin/python3 - <<'EOF'
import serial, time
s = serial.Serial('/dev/cu.usbserial-5B212355431', 115200, timeout=2)
time.sleep(0.5)
s.write(b'status\n')
time.sleep(0.5)
print(s.read(s.in_waiting).decode(errors='replace'))
s.close()
EOF
```

### Tip — see the full boot log

Open the terminal session **before** pressing reset to see every `[init]`,
`[wifi]`, `[sd]`, and `[ntp]` log line from startup.

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

### SD card not detected ("No SD" in vCON panel)

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

If an upload takes longer than 15 seconds (very slow WiFi), the mic pauses
while the previous chunk finishes uploading. Recording resumes automatically.
Check RSSI in the WIFI panel — signal below -75 dBm will cause slow uploads.

---

### Display stays on "Initializing..." / PSRAM error screen

Board FQBN must be `esp32:esp32:m5stack_core2`. Confirm you are not building
for a generic ESP32 or M5Stack Core (non-Core2).

---

## 13. Project Structure

```
vcon-m5-stack-audio/
├── README.md                         ← This file
├── CAPTIVE_PORTAL_IMPLEMENTATION_PLAN.md
└── VConRecorder/
    ├── VConRecorder.ino              ← Main Arduino sketch (~1 900 lines)
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
| `saveWavToSD(buf, n, uuid)` | Stream WAV from explicit buffer to SD |
| `saveVConToSD(uuid, json, len)` | Write JSON to SD |
| `buildAndUploadVConCore(buf, n)` | Encode + SD save + HTTP POST (no display calls) |
| `buildAndUploadVCon()` | Single-shot wrapper; calls core with globals |
| `uploadTaskFn(param)` | FreeRTOS task (core 0): runs core for continuous mode |
| `startUploadTask(buf, n)` | Launch upload task with given buffer |
| `swapAndContinue()` | Swap dual buffers + launch upload task each chunk |
| `allocateAudioBuffer()` | Allocate PSRAM buffer A at startup |
| `allocateContinuousBuffers()` | Allocate PSRAM buffer B on first RUN press |
| `startRecording()` | Initialise mic, set STATE_RECORDING or STATE_CONTINUOUS |
| `recordAudioChunk()` | Capture 1 024 samples each loop() iteration |
| `updateDisplay()` | Full screen redraw every 300 ms |
| `showLogoSplash()` | 10-second boot splash with device ID |
| `showConfig()` | Modal config screen (Button C) |
| `handleSerialCommand(cmd)` | Parse and execute serial commands |

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

*Updated 2026-04-01 — firmware: continuous dual-buffer mode, VConic gateway, SD rotation, OTA updates*
