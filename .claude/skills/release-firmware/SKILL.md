---
name: release-firmware
description: Increment version, compile, commit, push, and deploy firmware to the OTA server
allowed-tools: Bash, Read, Edit
---

Perform a full firmware release for the M5Stack vCon Recorder (Core2 and CoreS3). Follow these steps in order.

## 1. Determine the version bump

Read `VConRecorder/config.h` and extract the current `FIRMWARE_VERSION` string (e.g. `"1.0.8"`).

- **Default (no qualifier given):** patch increment → `1.0.8 → 1.0.9`
- **User says "minor":** minor increment, reset patch → `1.0.8 → 1.1.0`
- **User says "major":** major increment, reset minor + patch → `1.0.8 → 2.0.0`

Compute the new version string and show the user:
```
Current: 1.0.8
New:     1.0.9  (patch)
```

Ask for confirmation before proceeding.

## 2. Update the version in config.h

Edit `VConRecorder/config.h` — replace the `FIRMWARE_VERSION` line only:

```c
#define FIRMWARE_VERSION      "1.0.9"
```

Do not change anything else in the file.

## 3. Compile both board variants

Build for **both** Core2 and CoreS3:

```bash
# Core2
/opt/homebrew/bin/arduino-cli compile \
  --fqbn esp32:esp32:m5stack_core2 \
  --build-property "build.partitions=min_spiffs" \
  --build-property "upload.maximum_size=1966080" \
  --output-dir VConRecorder/build/esp32.esp32.m5stack_core2 \
  VConRecorder

# CoreS3
/opt/homebrew/bin/arduino-cli compile \
  --fqbn esp32:esp32:m5stack_cores3 \
  --build-property "build.partitions=min_spiffs" \
  --build-property "upload.maximum_size=1966080" \
  --output-dir VConRecorder/build/esp32.esp32.m5stack_cores3 \
  VConRecorder
```

Show the sketch size output for both. If either compilation fails, stop and report the error — do not proceed.

## 4. Commit and push to git

Stage and commit the updated config and both firmware binaries:

```bash
git add VConRecorder/config.h \
       VConRecorder/build/esp32.esp32.m5stack_core2/firmware.bin \
       VConRecorder/build/esp32.esp32.m5stack_cores3/firmware.bin
git commit -m "release: firmware v<NEW_VERSION>"
git push
```

Show the commit hash and confirm the push succeeded.

## 5. Confirm before OTA deploy

**Stop here.** Show the user a summary:

```
Ready to deploy to OTA server:
  Core2 binary:  VConRecorder/build/esp32.esp32.m5stack_core2/firmware.bin
  CoreS3 binary: VConRecorder/build/esp32.esp32.m5stack_cores3/firmware.bin
  Version:       <NEW_VERSION>
  Endpoints:     https://vcon-gateway.replit.app/api/ota/core2/
                 https://vcon-gateway.replit.app/api/ota/cores3/

Deploy order: firmware.bin is uploaded BEFORE version.txt is updated.
If you deploy version.txt first, devices will attempt to download a
mismatched binary and get stuck in a boot loop.

Proceed with OTA deploy? (yes / no)
```

Wait for explicit confirmation. If the user says no, stop — the git commit is already done and the binaries can be deployed manually later.

## 6. Get OTA credentials

Ask the user for their gateway credentials:
```
Gateway email:    <ask>
Gateway password: <ask>
```

Then obtain a JWT token:

```bash
TOKEN=$(curl -s -X POST https://vcon-gateway.replit.app/api/auth/login \
  -H "Content-Type: application/json" \
  -d "{\"email\":\"<EMAIL>\",\"password\":\"<PASSWORD>\"}" \
  | python3 -c "import sys,json; print(json.load(sys.stdin)['token'])")
echo "Token acquired: ${TOKEN:0:20}..."
```

If the token is empty or the login fails, stop and report the error.

## 7. Upload firmware binaries

Upload **both** board variants. Do NOT use `base64 -w0` (Linux-only flag):

```bash
for BOARD in core2 cores3; do
  python3 - <<PYEOF
import base64, json, urllib.request, os

binary_path = f"VConRecorder/build/esp32.esp32.m5stack_{os.environ['BOARD']}/firmware.bin"
token = os.environ.get("OTA_TOKEN", "")

with open(binary_path, "rb") as f:
    data = base64.b64encode(f.read()).decode("ascii")

payload = json.dumps({"firmwareBase64": data}).encode("utf-8")
req = urllib.request.Request(
    f"https://vcon-gateway.replit.app/api/ota/{os.environ['BOARD']}/firmware",
    data=payload,
    headers={"Authorization": f"Bearer {token}", "Content-Type": "application/json"},
    method="POST"
)
try:
    with urllib.request.urlopen(req, timeout=120) as resp:
        print(f"{os.environ['BOARD']} upload OK:", resp.read().decode())
except Exception as e:
    print(f"{os.environ['BOARD']} upload FAILED:", e)
    raise SystemExit(1)
PYEOF
done
```

Pass variables via environment:
```bash
OTA_TOKEN="$TOKEN" BOARD="core2" python3 - <<PYEOF ... PYEOF
OTA_TOKEN="$TOKEN" BOARD="cores3" python3 - <<PYEOF ... PYEOF
```

If either upload fails, stop — do NOT update the version strings.

## 8. Set the version strings

Only after **both** binary uploads succeed:

```bash
for BOARD in core2 cores3; do
  curl -s -X PUT "https://vcon-gateway.replit.app/api/ota/$BOARD/version" \
    -H "Authorization: Bearer $TOKEN" \
    -H "Content-Type: application/json" \
    -d "{\"version\":\"<NEW_VERSION>\"}"
done
```

## 9. Verify

Confirm all endpoints return the expected values (no auth required):

```bash
for BOARD in core2 cores3; do
  echo "=== $BOARD ==="
  echo "Version:" && curl -s "https://vcon-gateway.replit.app/api/ota/$BOARD/version.txt"
  echo "Binary size:" && curl -sI "https://vcon-gateway.replit.app/api/ota/$BOARD/firmware.bin" \
    | grep -i content-length
done
```

Report the results. A successful release looks like:
```
=== core2 ===
Version: 1.0.9
Binary size: content-length: 1425408
=== cores3 ===
Version: 1.0.9
Binary size: content-length: 1512960
```

---

## Key facts

- **arduino-cli path:** `/opt/homebrew/bin/arduino-cli` (not in PATH)
- **Supported boards:** Core2 (`esp32:esp32:m5stack_core2`) and CoreS3 (`esp32:esp32:m5stack_cores3`)
- **config.h path:** `VConRecorder/config.h`
- **Firmware binaries:** `VConRecorder/build/esp32.esp32.m5stack_<board>/firmware.bin`
- **OTA base URLs:** `https://vcon-gateway.replit.app/api/ota/core2/` and `.../cores3/`
- **Deploy order is critical:** binary upload BEFORE version string update, for BOTH boards
- **Do not use `base64 -w0`** — that flag is Linux-only; use Python for encoding on macOS
- **Do not store credentials** in any file — ask each session
- **Both boards share the same FIRMWARE_VERSION** — they are built from the same source
