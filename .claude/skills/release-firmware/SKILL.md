---
name: release-firmware
description: Increment version, compile, commit, push, and deploy firmware to the OTA server
allowed-tools: Bash, Read, Edit
---

Perform a full firmware release for the M5Stack Core2 vCon Recorder. Follow these steps in order.

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

## 3. Compile

```bash
/opt/homebrew/bin/arduino-cli compile \
  --fqbn esp32:esp32:m5stack_core2 \
  --output-dir VConRecorder/build \
  VConRecorder
```

Show the sketch size output. If compilation fails, stop and report the error — do not proceed.

The firmware binary will be at:
```
VConRecorder/build/esp32.esp32.m5stack_core2/firmware.bin
```

## 4. Commit and push to git

Stage and commit both the updated `config.h` and the new `firmware.bin`:

```bash
git add VConRecorder/config.h VConRecorder/build/esp32.esp32.m5stack_core2/firmware.bin
git commit -m "release: firmware v<NEW_VERSION>"
git push
```

Show the commit hash and confirm the push succeeded.

## 5. Confirm before OTA deploy

**Stop here.** Show the user a summary:

```
Ready to deploy to OTA server:
  Binary:   VConRecorder/build/esp32.esp32.m5stack_core2/firmware.bin
  Version:  <NEW_VERSION>
  Endpoint: https://vcon-gateway.replit.app/api/ota/

Deploy order: firmware.bin is uploaded BEFORE version.txt is updated.
If you deploy version.txt first, devices will attempt to download a
mismatched binary and get stuck in a boot loop.

Proceed with OTA deploy? (yes / no)
```

Wait for explicit confirmation. If the user says no, stop — the git commit is already done and the binary can be deployed manually later.

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

## 7. Upload the firmware binary

Upload via base64-encoded JSON body — do NOT use `base64 -w0` (Linux-only flag):

```bash
python3 - <<PYEOF
import base64, json, urllib.request, os

binary_path = "VConRecorder/build/esp32.esp32.m5stack_core2/firmware.bin"
token = os.environ.get("OTA_TOKEN", "")

with open(binary_path, "rb") as f:
    data = base64.b64encode(f.read()).decode("ascii")

payload = json.dumps({"firmwareBase64": data}).encode("utf-8")
req = urllib.request.Request(
    "https://vcon-gateway.replit.app/api/ota/firmware",
    data=payload,
    headers={"Authorization": f"Bearer {token}", "Content-Type": "application/json"},
    method="POST"
)
try:
    with urllib.request.urlopen(req, timeout=120) as resp:
        print("Upload OK:", resp.read().decode())
except Exception as e:
    print("Upload FAILED:", e)
    raise SystemExit(1)
PYEOF
```

Pass the token via environment variable:
```bash
OTA_TOKEN="$TOKEN" python3 - <<PYEOF
... (script above)
PYEOF
```

If the upload fails, stop — do NOT update the version string.

## 8. Set the version string

Only after the binary upload succeeds:

```bash
curl -s -X PUT https://vcon-gateway.replit.app/api/ota/version \
  -H "Authorization: Bearer $TOKEN" \
  -H "Content-Type: application/json" \
  -d "{\"version\":\"<NEW_VERSION>\"}"
```

## 9. Verify

Confirm both endpoints return the expected values (no auth required):

```bash
echo "Version:" && curl -s https://vcon-gateway.replit.app/api/ota/version.txt
echo "Binary size:" && curl -sI https://vcon-gateway.replit.app/api/ota/firmware.bin \
  | grep -i content-length
```

Report the results. A successful release looks like:
```
Version: 1.0.9
Binary size: content-length: 1425408
```

---

## Key facts

- **arduino-cli path:** `/opt/homebrew/bin/arduino-cli` (not in PATH)
- **FQBN:** `esp32:esp32:m5stack_core2`
- **config.h path:** `VConRecorder/config.h`
- **Firmware binary:** `VConRecorder/build/esp32.esp32.m5stack_core2/firmware.bin`
- **OTA base URL:** `https://vcon-gateway.replit.app/api/ota/`
- **Deploy order is critical:** binary upload BEFORE version string update
- **Do not use `base64 -w0`** — that flag is Linux-only; use Python for encoding on macOS
- **Do not store credentials** in any file — ask each session
