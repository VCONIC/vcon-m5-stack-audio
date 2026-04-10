---
name: flash-controller
description: Diagnose and flash firmware to a connected M5Stack Core2 or CoreS3 vCon Recorder device
allowed-tools: Bash, Read, Edit
---

Diagnose and/or update a connected M5Stack vCon Recorder device (Core2 or CoreS3). Follow these steps in order:

## 1. Find the device

Run `ls /dev/cu.usbserial-*` to discover connected USB serial ports. If multiple ports are found, list them and ask the user which one to use. If none are found, tell the user to connect the device and retry.

## 2. Determine the board type

Ask the user which board they have connected: **Core2** or **CoreS3**. Use this to set the FQBN and build directory for subsequent steps:

| Board | FQBN | Build dir |
|-------|------|-----------|
| Core2 | `esp32:esp32:m5stack_core2` | `VConRecorder/build/esp32.esp32.m5stack_core2/` |
| CoreS3 | `esp32:esp32:m5stack_cores3` | `VConRecorder/build/esp32.esp32.m5stack_cores3/` |

## 3. Release any busy port

Run `lsof /dev/cu.usbserial-*` to see if any process has the port open. If so, kill that process (`kill <pid>`) before proceeding — do not ask the user to do it.

## 4. Read current firmware version from device

Use pyserial to send the `status` command and capture the response:

```bash
python3 -c "
import serial, time
s = serial.Serial('/dev/cu.usbserial-XXXXX', 115200, timeout=3)
time.sleep(1.5)
s.write(b'status\n')
time.sleep(1)
print(s.read(s.in_waiting).decode(errors='ignore'))
s.close()
"
```

Parse the `Firmware:` line from the output to get the running version. If you cannot read serial output (e.g. device is in a boot loop), skip to step 6.

## 5. Compare versions

Read `VConRecorder/config.h` and extract `FIRMWARE_VERSION`. Compare against what the device reported:

- If already current: tell the user, and ask whether to flash anyway or stop.
- If outdated: report both versions and proceed to flash.

## 6. Flash the firmware

Run (substitute the correct FQBN and build dir from step 2):

```bash
/opt/homebrew/bin/arduino-cli upload \
  --fqbn <FQBN> \
  --port <port> \
  --input-dir <build_dir> \
  VConRecorder
```

Show progress. Confirm all three hash verifications passed. If it fails with "port busy", wait 2 seconds and retry once.

## 7. Verify after flash

Wait 4 seconds for the device to reboot, then re-read serial `status` and confirm the device now reports the expected firmware version.

## 8. Update memory if port changed

If the port used differs from the one recorded in the Claude Code project memory file (`memory/project_vcon_recorder.md` inside the project's `.claude` directory), update it so future sessions use the correct port without re-discovery.

---

## Key facts

- **arduino-cli**: `/opt/homebrew/bin/arduino-cli` (not in PATH)
- **Supported boards**: Core2 (`esp32:esp32:m5stack_core2`) and CoreS3 (`esp32:esp32:m5stack_cores3`)
- **Pre-built binaries**: `VConRecorder/build/esp32.esp32.m5stack_<board>/firmware.bin`
- **Baud rate**: 115200
- **Flash the pre-built binary** — do not recompile unless the user explicitly asks
- **Boot loop recovery**: skip serial read, go straight to flashing
