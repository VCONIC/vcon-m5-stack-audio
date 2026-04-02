# VConic Recorder — User Guide

**Firmware 1.0.6 · M5Stack Core2**

The VConic Recorder captures audio from a small device on a desk or shelf and
continuously uploads each chunk as a structured
[vCon](https://datatracker.ietf.org/doc/draft-ietf-vcon-vcon-core/) conversation
record to the VConic portal. Every recording is also saved to a microSD card so
nothing is lost if the network is unavailable.

---

## Table of Contents

1. [What's in the Box](#1-whats-in-the-box)
2. [First-Time Setup](#2-first-time-setup)
3. [Connecting via Serial Terminal](#3-connecting-via-serial-terminal)
4. [WiFi Configuration](#4-wifi-configuration)
5. [Portal Token Setup](#5-portal-token-setup)
6. [The Display — Screen by Screen](#6-the-display--screen-by-screen)
7. [Recording](#7-recording)
8. [Adjusting Recording Duration](#8-adjusting-recording-duration)
9. [Serial Command Reference](#9-serial-command-reference)
10. [SD Card Storage](#10-sd-card-storage)
11. [OTA Firmware Updates](#11-ota-firmware-updates)
12. [Troubleshooting](#12-troubleshooting)
13. [Quick Reference](#13-quick-reference)

---

## 1. What's in the Box

- **M5Stack Core2** device with VConic firmware pre-installed
- USB-C cable for power and serial configuration
- (Optional) microSD card — formatted FAT32, inserted in the slot on the side

**Ports and controls:**

```
┌─────────────────────────────────┐
│         320×240 display         │
│                                 │
│  [  Button A  ][  B  ][  C  ]   │  ← capacitive touch (below screen)
└────────────────────────────────-┘
        │ USB-C (power + serial)
        │ microSD slot (right side)
```

| Button | Default label | Notes |
|--------|--------------|-------|
| A (left) | **RUN** / **HOME** / **LESS** | Context-sensitive |
| B (center) | **STOP** / **CONFIG** / **SAVE** | Context-sensitive |
| C (right) | **TOOLS** / **STATUS** / **MORE** | Context-sensitive |

Power is supplied via USB-C. The device has an internal battery — it will run
for a short time unplugged, but keep it plugged in for continuous operation.

---

## 2. First-Time Setup

### Step 1 — Power on

Connect the USB-C cable. The VConic logo and device ID (`VC-XXXXXX`) appear for
a few seconds, then the **HOME** screen loads.

The device ID is unique to your unit. Write it down — you'll need it when
registering in the portal.

### Step 2 — Insert a microSD card (recommended)

Format the card as **FAT32** on your computer, then insert it into the slot on
the right side of the device. The device saves a WAV file and a JSON record of
every recording to the card, even when offline.

> The device boots and records normally without a card. SD features are simply
> skipped if no card is present.

### Step 3 — Connect via serial and configure WiFi

See [§3 Connecting via Serial Terminal](#3-connecting-via-serial-terminal) and
[§4 WiFi Configuration](#4-wifi-configuration).

### Step 4 — Set your portal token (optional but recommended)

See [§5 Portal Token Setup](#5-portal-token-setup).

### Step 5 — Press RUN

Once WiFi is connected and the clock shows a real time, press **Button A (RUN)**
on the HOME screen. Recording begins immediately.

---

## 3. Connecting via Serial Terminal

The device is configured through a serial terminal at **115200 baud, 8N1**. You
need this once to set WiFi credentials. After that the device is self-sufficient.

### macOS

1. Plug in the USB-C cable.
2. Find the port:
   ```bash
   ls /dev/cu.* | grep usb
   ```
   You'll see something like `/dev/cu.usbserial-5B212355431`.

3. Open a terminal session:
   ```bash
   screen /dev/cu.usbserial-5B212355431 115200,crnl
   ```

4. Press **Enter** — you should see the `>` prompt or device log lines.

   - **Local echo** (see what you type): press `Ctrl-A` then `E`
   - **Exit screen**: press `Ctrl-A` then `K`, then `Y`

**Alternative — arduino-cli monitor:**
```bash
arduino-cli monitor --port /dev/cu.usbserial-5B212355431 --config baudrate=115200
```
Exit with `Ctrl-C`. Must be closed before flashing firmware.

---

### Windows

1. Plug in the USB-C cable.
2. Open **Device Manager**:
   - Press **Win + X** and choose *Device Manager*, or
   - Press **Win + R**, type `devmgmt.msc`, press Enter.

   Expand **Ports (COM & LPT)**.
   Look for **USB-SERIAL CH340** or **Silicon Labs CP210x** — note the COM
   number, e.g. `COM5`.

   > If the entry has a yellow warning triangle, right-click it and choose
   > *Update driver*.

   > If no port appears, install the
   > [CH340 driver](https://www.wch-ic.com/downloads/CH341SER_EXE.html) or
   > [CP210x driver](https://www.silabs.com/developers/usb-to-uart-bridge-vcp-drivers).

3. Open a terminal with one of these options:

   **PuTTY** (recommended for beginners):
   - Download from [putty.org](https://www.putty.org)
   - Connection type: **Serial**
   - Serial line: `COM5` (use your actual COM number)
   - Speed: `115200`
   - Click **Open**

   **Windows Terminal / PowerShell:**
   ```powershell
   # Install if needed:
   winget install PuTTY.PuTTY
   ```

   **Built-in (no install):**
   ```powershell
   mode COM5 BAUD=115200 PARITY=N DATA=8 STOP=1
   # Then open: Start → Device Manager → right-click port → Open
   ```

4. Press **Enter** — you should see the prompt or log output.

---

### Linux

1. Plug in the USB-C cable.
2. Find the port:
   ```bash
   ls /dev/ttyUSB* /dev/ttyACM* 2>/dev/null
   ```
   Typically `/dev/ttyUSB0`.

3. Add yourself to the `dialout` group if you get a permission error:
   ```bash
   sudo usermod -aG dialout $USER
   # Log out and back in for this to take effect
   ```

4. Open a terminal session:
   ```bash
   screen /dev/ttyUSB0 115200,crnl
   ```
   Exit: `Ctrl-A` then `K` then `Y`.

   **Alternative — minicom:**
   ```bash
   sudo apt install minicom   # Debian/Ubuntu
   minicom -D /dev/ttyUSB0 -b 115200
   ```
   Exit: `Ctrl-A` then `X`.

   **Alternative — Python (scripted/non-interactive):**
   ```bash
   python3 -m venv /tmp/venv && /tmp/venv/bin/pip install pyserial -q
   /tmp/venv/bin/python3 - <<'EOF'
   import serial, time
   s = serial.Serial('/dev/ttyUSB0', 115200, timeout=3)
   time.sleep(0.5)
   s.write(b'status\r\n')
   time.sleep(2)
   print(s.read(s.in_waiting or 4096).decode(errors='replace'))
   s.close()
   EOF
   ```

---

### Verifying the connection

Once connected, type `status` and press Enter. You should see:

```
=== vCon Recorder Status ===
Firmware:    1.0.6
Device ID:   VC-832924
MAC Address: 84:1F:E8:83:29:24
Rec Duration:45 s (continuous mode, range 10-120 s)
...
============================
```

If you see nothing, try pressing **Enter** or **Reset** on the device. If only
garbled characters appear, check that the baud rate is set to **115200**.

---

## 4. WiFi Configuration

The device connects to a 2.4 GHz WiFi network. **5 GHz networks are not
supported.**

In the serial terminal, type:

```
wifi YourNetworkName YourPassword
```

The device reconnects immediately. Credentials are saved to internal flash and
used on every subsequent boot — you only need to do this once.

**If your network name contains spaces**, the last word is always treated as the
password. Everything before the last word is the SSID — this works for any
number of words:

```
wifi Barnhill Tavern meteormeteor
     ↑─────── SSID ──────┘ ↑ password

wifi My Coffee Shop Network s3cr3t
     ↑──────── SSID ───────────┘ ↑ password
```

> **Special characters** (e.g. `!`, `@`, `#`, `$`) in SSIDs or passwords may
> not work correctly due to serial terminal encoding. If your credentials
> contain special characters and the connection fails, try temporarily
> switching to a network that uses only letters, digits, and hyphens.

**Verify it connected:**

After sending the `wifi` command, wait about 5 seconds, then type:
```
status
```
Look for `WiFi Status: Connected` and a valid IP address in the output. If it
shows `Disconnected`, double-check the SSID and password (they are
case-sensitive) and try again.

**To scan for available networks:**
```
scan
```
This lists nearby 2.4 GHz networks with signal strength and security type.

---

## 5. Portal Token Setup

A portal token links your device to your VConic account so recordings are
automatically routed to your workspace.

### Without a token

Without a token the device still records and uploads, but recordings arrive as
**unassigned** (HTTP 200). You can claim them in the portal by matching the
device's MAC address, but this is a manual step and must be done before the
portal purges unclaimed recordings (retention policy varies by account).

### Getting your token

1. Log in to the [VConic portal](https://vcon-gateway.replit.app).
2. Navigate to **Devices → Add Device** (or your account settings).
3. Copy the device token — it looks like `dvt_abc123xyz`.

### Setting the token on the device

In the serial terminal:
```
token dvt_abc123xyz
```

The token is saved to flash immediately and persists through reboots and OTA
firmware updates — you only need to set it once. From this point, every
recording uploaded from this device returns HTTP 202 (routed to your account)
instead of HTTP 200.

**To clear the token** (revert to MAC-address routing):
```
token
```
(no argument)

**To verify the token is set:**
```
status
```
Look for `Device Token: dvt_abc123xyz` in the output.

---

## 6. The Display — Screen by Screen

The device has five screens. Buttons at the bottom of the display change
function depending on which screen is active — labels are always shown in
the button row.

Any screen other than HOME returns to HOME automatically after **60 seconds**
of no button activity.

---

### HOME Screen

The default view. Designed for a quick glance across a room.

```
┌──── green hazard strip ─────────────────────────────────┐
│  VC-832924                                   ● WiFi      │
│                                                          │
│                        IDLE                             │
│                  (colour-coded state)                   │
│                                                          │
├──────────────────────────────────────────────────────────┤
│  OK: 150                              Err: 8             │
├──────────────────────────────────────────────────────────┤
│  Press RECORD to begin          dur:45s   HTTP 202       │
├──────────────────────────────────────────────────────────┤
│  [ RUN ]              [ CONFIG ]              [ TOOLS ]  │
└──────────────────────────────────────────────────────────┘
```

**State colours:**

| State | Background | Meaning |
|-------|-----------|---------|
| **IDLE** | Near-black | Waiting, ready to record |
| **LIVE** | Red | Recording in continuous mode |
| **RECORDING** | Red | Recording in single-shot mode |
| **ENCODING** | Dark teal | Compressing audio to WAV/base64 |
| **UPLOADING** | Dark teal | Sending to the portal |
| **SUCCESS** | Dark green | Last upload succeeded |
| **ERROR** | Red | Last upload failed |

**Button labels change with state:**

| State | Button A | Button B | Button C |
|-------|---------|---------|---------|
| Idle / Success / Error | **RUN** | **CONFIG** | **TOOLS** |
| Recording or Live | _(inactive)_ | **STOP** | **STATUS** |
| Encoding / Uploading | _(inactive)_ | _(inactive)_ | **STATUS** |

---

### STATUS Screen

Real-time recording dashboard. Access it by pressing **Button C (STATUS)**
while recording, or navigate to it from other screens.

Contains (top to bottom):
- **VU meter** — live microphone level bars (green → yellow → red)
- **Progress bar** — elapsed time in current chunk
- **Upload state** — ENCODING / SD WRITE / POSTING / OK / FAILED
- **Counters** — total OK posts, errors, and chunk number
- **WiFi** — SSID, IP address, signal strength (dBm)
- **Time / Date** — UTC clock and date
- **Uptime / PSRAM** — how long since last boot, free memory
- **Mode / SD** — recording mode and number of files saved to card

Button **A (HOME)** returns to the main screen.

---

### CONFIG Screen

Read-only view of all current settings — useful for checking configuration
without opening a serial terminal.

Shows: device ID, MAC, firmware version, portal token, WiFi status, IP, RSSI,
NTP sync, recording duration and mode, POST URL.

Button **B (SET DUR)** opens the Duration Picker (see [§8](#8-adjusting-recording-duration)).
Button **A (HOME)** returns.

---

### TOOLS Screen

On-device diagnostics. Run tests without a computer.

| Tool | What it does |
|------|-------------|
| **Test WiFi** | Reconnects and reports IP + signal strength |
| **Test OTA** | Checks the firmware update server (no flash/reboot) |
| **Test POST** | Sends a 1-second silent recording; reports HTTP response code |
| **Show SD** | Displays SD card total / used / free space |
| **Restart** | Reboots the device (asks for confirmation) |

**Navigation:**
- **Button A** — move cursor up through the menu
- **Button B (RUN)** — run selected tool
- **Button C (HOME)** — return to HOME

Results display for 8 seconds, then return to the menu automatically.

---

### DURATION Screen

Interactive recording length picker. Reached via **CONFIG → SET DUR**.

- **Button A (LESS)** — decrease by 5 seconds
- **Button C (MORE)** — increase by 5 seconds
- **Button B (SAVE)** — save to flash and return to CONFIG

The display shows the current value as a large number, a range bar from 10–120 s,
and whether the selected duration enables **continuous** or **single-shot** mode
(see [§7 Recording](#7-recording)). The cyan tick on the bar marks the 45 s
continuous-mode threshold.

Changes are discarded if you navigate away without pressing SAVE.

---

## 7. Recording

### Starting a recording

From the HOME screen, press **Button A (RUN)**.

The device:
1. Checks WiFi and reconnects if needed
2. Allocates audio buffers in PSRAM
3. Starts the microphone
4. Begins filling the buffer

### Continuous mode vs single-shot mode

The recording mode is determined automatically by the chunk duration setting:

| Duration | Mode | What happens after each chunk |
|----------|------|-------------------------------|
| 10–45 seconds | **Continuous** | Core 1 instantly starts recording the next chunk. Core 0 simultaneously encodes, saves to SD, and uploads the previous chunk. Zero gap between recordings. |
| 46–120 seconds | **Single-shot** | Device records one chunk, then encodes, saves, and uploads before starting the next. There is a brief pause between chunks — typically **5–15 seconds** depending on chunk length and network speed. |

The default duration is **45 seconds** — continuous mode is active out of the box.

The STATE on the HOME screen shows **LIVE** in continuous mode and
**RECORDING** in single-shot mode.

### Stopping

Press **Button B (STOP)** from the HOME screen while recording.

- **Continuous mode:** finishes uploading the current chunk, then stops.
- **Single-shot mode:** immediately stops recording and encodes what was captured.

Audio already captured is never discarded — even a partial chunk is encoded, saved to SD, and uploaded.

### What happens to each recording

For every chunk the device:

1. **Saves the WAV file** to SD card (always, even if upload fails)
2. **Saves the vCon JSON** to SD card
3. **HTTP POSTs the vCon** to the configured portal endpoint
4. Reports the HTTP response code on the STATUS screen

If the upload fails, the device retries automatically up to 3 times with
exponential back-off (5 s → 30 s → 5 min). After 3 failed attempts the
recording is marked as a failed upload and **not retried again** — but the WAV
and vCon JSON are always preserved on the SD card regardless of upload outcome,
so no audio is ever lost.

### Upload response codes

| Code | Meaning |
|------|---------|
| **202** | Recording routed to your account (token is set and valid) |
| **200** | Recording accepted as unassigned (no token set) |
| **400** | Bad payload — check serial output for encoding errors |
| **5xx** | Server error — device will retry automatically |

---

## 8. Adjusting Recording Duration

### On the device

1. From HOME, press **Button B (CONFIG)**
2. Press **Button B (SET DUR)**
3. Use **Button A (LESS)** and **Button C (MORE)** to adjust in 5-second steps
4. Press **Button B (SAVE)** to store

Valid range: **10–120 seconds**. The picker shows whether your selection uses
continuous or single-shot mode.

### Via serial terminal

```
dur 30     # set to 30 seconds
dur 45     # set to 45 seconds (default, continuous mode)
dur 90     # set to 90 seconds (single-shot mode)
dur        # show current value without changing it
```

Changes take effect on the next RUN press. The setting is saved to flash and
survives reboots.

> **Cannot be changed while recording** — press STOP first.

---

## 9. Serial Command Reference

Connect at **115200 baud, 8N1**. Commands are case-sensitive, terminated with Enter.

---

### `wifi <ssid> <password>`

Set WiFi credentials and immediately reconnect. The last word is always the
password; everything before it is the SSID.

```
wifi HomeNetwork MyPassword123
wifi Barnhill Tavern meteormeteor
```

Saved to flash. Takes effect immediately.

---

### `token <value>`

Set the portal device token.

```
token dvt_abc123xyz
```

---

### `token`

Clear the token. Device reverts to MAC-address routing (HTTP 200).

---

### `url <endpoint>`

Set the vCon upload endpoint.

```
url https://vcon-gateway.replit.app/ingress
url https://api.yourcompany.com/vcons
```

Saved to flash immediately.

---

### `dur <seconds>`

Set recording chunk length (10–120 s).

```
dur 45
```

---

### `dur`

Show the current duration without changing it.

---

### `status`

Print a full status report showing firmware version, device ID, MAC, WiFi,
NTP, upload counters, free memory, and all configuration values.

---

### `scan`

Scan for nearby 2.4 GHz networks and print SSID, RSSI, and security type.

---

### `sd`

Show SD card statistics: total space, used space, free space, and recordings
saved this session.

---

### `restart`

Soft-reboot the device. WiFi reconnects and OTA check runs on the next boot.

---

### `help`

Print all available commands.

---

## 10. SD Card Storage

### Setup

1. Format the card as **FAT32** on your computer.
   - Windows: right-click the drive → Format → FAT32
   - macOS: Disk Utility → Erase → MS-DOS (FAT)
   - Linux: `sudo mkfs.fat -F32 /dev/sdX`
2. Insert into the slot on the right side of the Core2.
3. Power-cycle or type `restart` in the serial terminal.

The serial output confirms the card at boot:
```
[sd] Ready — 59607 MB total, 106 MB used
[sd] Card type: SDHC
```

### Directory layout

```
/
├── wav/
│   └── 2026/04/01/14/
│       ├── a54553f6-...-uuid.wav   ← raw RIFF/WAV, 16-bit mono, 8 kHz
│       └── ...
└── vcons/
    └── 2026/04/01/14/
        ├── a54553f6-...-uuid.json  ← complete vCon JSON record
        └── ...
```

Files are organized by year/month/day/hour. You can pull the card, mount it on
a computer, and access recordings directly.

### Automatic space management

Before each save, the device checks free space. If less than **20 MB** remains,
it deletes the oldest hour-directory under `/wav` and `/vcons` until enough
space is available. This runs silently. You will see log messages in the serial
terminal:

```
[sd] low space (4 MB free) — pruning old files
[sd] pruning: /wav/2026/03/31/14
```

### Save order

The device always saves to SD **before** attempting the network upload. If the
upload fails, the recording is still on the card.

---

## 11. OTA Firmware Updates

The device checks for a firmware update on **every boot**, after WiFi connects.
Updates are downloaded and installed automatically — no USB cable or computer
required.

### What you see in the serial terminal

**No update available:**
```
[OTA] local=1.0.6  remote=1.0.6
[OTA] firmware is current
```

**Update found and installed:**
```
[OTA] local=1.0.5  remote=1.0.6
[OTA] update available (1.0.5 → 1.0.6), downloading...
[OTA] binary size: 1418976 bytes
[OTA]  141897 / 1418976 bytes (10%)
[OTA]  283794 / 1418976 bytes (20%)
  ...
[OTA] 1418976 / 1418976 bytes (100%)
[OTA] success — rebooting in 1 s...
```

**What you see on screen:** The device stays on the boot splash (VConic logo
and device ID) throughout the download. Progress percentages appear only in the
serial terminal. A typical update takes **30–60 seconds** over a normal WiFi
connection.

The device reboots automatically into the new firmware. The full boot sequence
then runs again: WiFi connects, the OTA check runs and confirms
`local == remote` (no loop), NTP syncs, and the HOME screen appears.

### How it works

The device polls two files on the update server:

1. **`version.txt`** — a plain-text string (e.g. `1.0.6`). If this matches the
   firmware baked into the device, nothing happens.
2. **`firmware.bin`** — the raw ESP32 binary. Downloaded and flashed into the
   inactive OTA partition only if the version differs.

The ESP32 has two equal-sized app partitions. The currently running firmware is
never overwritten — the update lands in the standby partition and becomes active
after reboot. If the update fails at any point, the device boots from the
original partition unchanged.

### OTA failure modes

| Situation | Behaviour |
|-----------|-----------|
| No WiFi | OTA check skipped entirely; normal startup continues |
| Server returns HTTP 404 | Logged as error; normal startup continues |
| Download interrupted | Partition not activated; device reboots on original firmware |
| Hash mismatch | Same as interrupted — original firmware intact |

### Checking your firmware version

```
status
```
Look for `Firmware: 1.0.6` (or whichever version is current).

---

## 12. Troubleshooting

### Device powers on but shows no WiFi

**Symptom:** HOME screen shows `✗ WiFi` in the corner.

1. Confirm you are using a **2.4 GHz** network — 5 GHz is not supported.
2. In the serial terminal, type `scan` to list visible networks.
3. Re-enter credentials:
   ```
   wifi YourSSID YourPassword
   ```
4. If still failing, type `restart` and watch the `[wifi]` log lines in the
   serial output.

---

### Serial terminal shows garbled characters

The baud rate is wrong. Disconnect, confirm the terminal is set to **115200**,
and reconnect.

---

### "Port in use" or "Permission denied" when opening serial terminal

Another program is holding the port.

- **macOS/Linux:**
  ```bash
  pkill -f screen
  pkill -f "arduino-cli monitor"
  ```
- **Windows:** Check Device Manager for competing apps, or reboot.

---

### NTP not synced — timestamps show 1970-01-01

WiFi must be connected before NTP can sync. Connect WiFi first, then:
```
restart
```

---

### Recordings show HTTP 200 instead of HTTP 202

The device has no portal token, so recordings arrive as unassigned. Set one:
```
token dvt_your_token_here
```

---

### OTA check returns HTTP 404 on every boot

The firmware update server does not yet have a binary uploaded for this device.
This is harmless — the device continues to operate normally. Contact your
administrator.

---

### SD card not detected

1. **Re-seat the card** — push it in firmly until it clicks, then type `restart`.
2. **Check the serial output** — connect a serial terminal and look for `[sd]`
   lines at boot. A working card shows:
   ```
   [sd] Ready — 59607 MB total, 106 MB used
   ```
   If there are no `[sd]` lines at all, the card is not being detected (go to
   step 3). If you see an error like `[sd] mount failed`, the filesystem is
   the problem (go to step 4).
3. **Try a different card** — some no-name cards are not recognised by the
   ESP32's SPI driver.
4. **Reformat as FAT32** — the card must be FAT32, not exFAT or NTFS.
   - Windows: right-click the drive → Format → FAT32. For cards larger than
     32 GB, Windows's built-in tool refuses; use
     [guiformat](http://ridgecrop.co.uk/index.htm?guiformat.htm) instead.
   - macOS: Disk Utility → Erase → MS-DOS (FAT)
   - Linux: `sudo mkfs.fat -F32 /dev/sdX`

---

### Upload returns HTTP 400

The vCon JSON payload has a structural problem. This typically indicates a
firmware issue. Check the serial output for encoding errors and note the firmware
version (`status`). Contact your administrator with the serial log.

---

### Recording stops unexpectedly / ERROR state

The most common cause is a PSRAM allocation failure. This can happen if:
- The duration is set very high (> 90 s)
- The device has been running for a long time without a reboot

Type `restart` to recover. If it recurs, reduce the duration:
```
dur 45
```

---

### Device is unresponsive to buttons

The capacitive touch buttons occasionally mis-detect in high-humidity
environments. Clean the area below the screen with a dry cloth. If the device
is completely locked, hold the power button (left side) for 6 seconds to force
a power cycle.

---

## 13. Quick Reference

### Button summary

| Screen | Button A | Button B | Button C |
|--------|---------|---------|---------|
| HOME (idle) | **RUN** — start recording | **CONFIG** — open config | **TOOLS** — diagnostics |
| HOME (recording) | — | **STOP** | **STATUS** |
| HOME (processing) | — | — | **STATUS** |
| STATUS | **HOME** | — | — |
| CONFIG | **HOME** | **SET DUR** — open duration picker | — |
| TOOLS (menu) | **PREV** — move up | **RUN** — run tool | **HOME** |
| TOOLS (result) | — | **BACK** | **HOME** |
| DURATION | **LESS** (−5 s) | **SAVE** — store & return | **MORE** (+5 s) |

---

### Serial commands

| Command | Effect |
|---------|--------|
| `wifi <ssid> <password>` | Set WiFi credentials |
| `token <value>` | Set portal device token |
| `token` | Clear token |
| `url <endpoint>` | Set upload endpoint |
| `dur <seconds>` | Set recording duration (10–120 s) |
| `dur` | Show current duration |
| `status` | Full status report |
| `scan` | Scan for WiFi networks |
| `sd` | SD card statistics |
| `restart` | Reboot device |
| `help` | List all commands |

---

### Recording modes

| Duration | Mode | Gap between chunks |
|----------|------|-------------------|
| 10–45 s | Continuous | None (zero-gap dual-buffer) |
| 46–120 s | Single-shot | Brief pause while encoding/uploading |

---

### LED / state colours on HOME screen

| Colour | State |
|--------|-------|
| Near-black (green text) | IDLE |
| Red | LIVE, RECORDING, or ERROR |
| Dark teal (cyan text) | ENCODING or UPLOADING |
| Dark green | SUCCESS |

---

*VConic Recorder User Guide · Firmware 1.0.6 · Updated 2026-04-02*
