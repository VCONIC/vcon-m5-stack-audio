#!/usr/bin/env bash
# test_device.sh — M5Stack Core2 vCon Recorder device test suite
# Usage: ./test_device.sh <serial_port> [baud_rate]
# Example: ./test_device.sh /dev/cu.usbserial-5B212355431

set -uo pipefail

PORT="${1:?Usage: $0 <serial_port> [baud_rate]}"
BAUD="${2:-115200}"
PASS=0
FAIL=0

RED=$'\e[31m'; GRN=$'\e[32m'; YLW=$'\e[33m'; CYN=$'\e[36m'; BLD=$'\e[1m'; RST=$'\e[0m'

pass()     { echo "${GRN}${BLD}[PASS]${RST} $*"; ((PASS++)) || true; }
fail()     { echo "${RED}${BLD}[FAIL]${RST} $*"; ((FAIL++)) || true; }
info()     { echo "${YLW}[INFO]${RST} $*"; }
test_hdr() { echo ""; echo "${CYN}${BLD}━━━ $* ━━━${RST}"; }

VENV="/tmp/vcon_test_venv"

# Setup pyserial venv once
if [ ! -f "$VENV/bin/python3" ]; then
    info "Setting up pyserial venv..."
    python3 -m venv "$VENV" 2>/dev/null
    "$VENV/bin/pip" install pyserial -q
fi

# send_cmd <command> <wait_seconds>
# Sends a serial command and returns the response
send_cmd() {
    local cmd="$1"
    local wait="${2:-1.0}"
    "$VENV/bin/python3" - "$PORT" "$BAUD" "$cmd" "$wait" <<'PYEOF'
import sys, serial, time
port, baud, cmd, wait = sys.argv[1], int(sys.argv[2]), sys.argv[3], float(sys.argv[4])
try:
    s = serial.Serial(port, baud, timeout=1)
    time.sleep(0.3)
    s.reset_input_buffer()
    if cmd:
        s.write((cmd + '\n').encode())
    time.sleep(wait)
    out = b''
    while s.in_waiting:
        out += s.read(s.in_waiting)
        time.sleep(0.05)
    s.close()
    print(out.decode('utf-8', errors='replace'))
except Exception as e:
    print(f"ERROR: {e}", file=sys.stderr)
    sys.exit(1)
PYEOF
}

echo ""
echo "${BLD}M5Stack Core2 vCon Recorder — Device Test Suite${RST}"
echo "Port: $PORT  Baud: $BAUD"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"

# ── Test 1–5: status command ──────────────────────────────────────────────────
test_hdr "Test 1-5: status command"
STATUS_OUT=$(send_cmd "status" 2.0)

echo "$STATUS_OUT" | grep -q "Firmware:"    \
    && pass "Firmware field present"         \
    || fail "Firmware field missing"

echo "$STATUS_OUT" | grep -q "WiFi Status:" \
    && pass "WiFi Status field present"      \
    || fail "WiFi Status field missing"

echo "$STATUS_OUT" | grep -q "NTP Synced:"  \
    && pass "NTP Synced field present"       \
    || fail "NTP Synced field missing"

echo "$STATUS_OUT" | grep -q "POST URL:"    \
    && pass "POST URL field present"         \
    || fail "POST URL field missing"

echo "$STATUS_OUT" | grep -q "Rec Duration:"\
    && pass "Rec Duration field present"     \
    || fail "Rec Duration field missing"

# ── Test 6: firmware version format ──────────────────────────────────────────
test_hdr "Test 6: firmware version format"
FIRMWARE=$(echo "$STATUS_OUT" | grep "Firmware:" | awk '{print $2}' | tr -d '\r')
if [[ "$FIRMWARE" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
    pass "Firmware version valid: $FIRMWARE"
else
    fail "Firmware version invalid or missing: '$FIRMWARE'"
fi

# ── Test 7: WiFi connected ────────────────────────────────────────────────────
test_hdr "Test 7: WiFi connectivity"
echo "$STATUS_OUT" | grep -q "WiFi Status:.*Connected" \
    && pass "WiFi is connected"                          \
    || fail "WiFi not connected"

# ── Test 8: NTP synced ────────────────────────────────────────────────────────
test_hdr "Test 8: NTP sync"
echo "$STATUS_OUT" | grep -q "NTP Synced:.*Yes" \
    && pass "NTP is synced"                       \
    || fail "NTP not synced"

# ── Test 9: dur command ───────────────────────────────────────────────────────
test_hdr "Test 9: dur command"
DUR_OUT=$(send_cmd "dur" 1.0)
echo "$DUR_OUT" | grep -qi "duration" \
    && pass "dur response contains 'duration'"  \
    || fail "dur response unexpected: $DUR_OUT"

# ── Test 10: url command ──────────────────────────────────────────────────────
test_hdr "Test 10: url set and verify"
ORIGINAL_URL=$(echo "$STATUS_OUT" | grep "POST URL:" | sed 's/.*POST URL: *//')
TEST_URL="https://vcon-gateway.replit.app/ingress"
send_cmd "url $TEST_URL" 0.5 > /dev/null
STATUS2=$(send_cmd "status" 1.5)
echo "$STATUS2" | grep -q "$TEST_URL" \
    && pass "url accepted and reflected in status"  \
    || fail "url not reflected in status"

# ── Test 11: help completeness ────────────────────────────────────────────────
test_hdr "Test 11: help command completeness"
HELP_OUT=$(send_cmd "help" 1.0)
for kw in wifi url token dur status sd restart help; do
    echo "$HELP_OUT" | grep -qi "$kw" \
        && pass "help contains '$kw'"   \
        || fail "help missing '$kw'"
done

# ── Test 12: unknown command ──────────────────────────────────────────────────
test_hdr "Test 12: unknown command handling"
UNK_OUT=$(send_cmd "xyzzy_notacommand" 0.8)
echo "$UNK_OUT" | grep -qi "unknown\|help" \
    && pass "Unknown command handled gracefully"  \
    || fail "Unknown command not handled: $UNK_OUT"

# ── Test 13-14: OTA endpoints ─────────────────────────────────────────────────
test_hdr "Test 13-14: OTA server endpoints"
OTA_VER_URL="https://vcon-gateway.replit.app/api/ota/version.txt"
OTA_FW_URL="https://vcon-gateway.replit.app/api/ota/firmware.bin"

VER_HTTP=$(curl -s -o /tmp/ota_ver_body.txt -w "%{http_code}" \
           --max-time 10 "$OTA_VER_URL" 2>/dev/null || echo "000")
[[ "$VER_HTTP" == "200" ]] \
    && pass "OTA version URL returns HTTP 200"  \
    || fail "OTA version URL returned HTTP $VER_HTTP"

VER_BODY=$(cat /tmp/ota_ver_body.txt 2>/dev/null | tr -d '[:space:]')
[[ -n "$VER_BODY" && "$VER_BODY" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] \
    && pass "OTA version body is valid: '$VER_BODY'"  \
    || fail "OTA version body invalid or empty: '$VER_BODY'"

FW_HTTP=$(curl -sI --max-time 10 "$OTA_FW_URL" 2>/dev/null || echo "")
echo "$FW_HTTP" | grep -qi "200 ok" \
    && pass "OTA firmware URL returns HTTP 200"  \
    || fail "OTA firmware URL not 200"

echo "$FW_HTTP" | grep -qi "content-length:" \
    && pass "OTA firmware response has Content-Length"  \
    || fail "OTA firmware missing Content-Length header"

# ── Test 15-17: restart and reboot ───────────────────────────────────────────
test_hdr "Test 15-17: restart and reboot detection"
info "Sending restart command..."
send_cmd "restart" 0.3 > /dev/null

info "Waiting for device to reboot (up to 25 s)..."
REBOOT_DETECTED=0
for i in $(seq 1 25); do
    sleep 1
    CHECK=$("$VENV/bin/python3" - "$PORT" "$BAUD" <<'PYEOF' 2>/dev/null || echo "")
import sys, serial, time
port, baud = sys.argv[1], int(sys.argv[2])
try:
    s = serial.Serial(port, baud, timeout=1)
    time.sleep(0.5)
    out = b''
    t = time.time()
    while time.time() - t < 2:
        if s.in_waiting:
            out += s.read(s.in_waiting)
        time.sleep(0.1)
    s.close()
    print(out.decode('utf-8', errors='replace'))
except:
    pass
PYEOF
    if echo "$CHECK" | grep -q "M5Stack\|vCon Recorder"; then
        REBOOT_DETECTED=1
        break
    fi
done

[[ $REBOOT_DETECTED -eq 1 ]] \
    && pass "Device rebooted — boot banner detected"  \
    || fail "Device did not reboot or boot banner not seen within 25 s"

# Allow full boot sequence (10s splash + WiFi + NTP)
info "Waiting for full boot sequence (30 s)..."
sleep 30

STATUS3=$(send_cmd "status" 2.0)
FIRMWARE2=$(echo "$STATUS3" | grep "Firmware:" | awk '{print $2}' | tr -d '\r')
[[ "$FIRMWARE2" == "$FIRMWARE" ]] \
    && pass "Post-reboot firmware matches pre-restart: $FIRMWARE2"  \
    || fail "Firmware mismatch post-reboot: was '$FIRMWARE', now '$FIRMWARE2'"

echo "$STATUS3" | grep -q "WiFi Status:.*Connected" \
    && pass "WiFi re-connected after reboot"           \
    || fail "WiFi not connected after reboot"

# ── Summary ───────────────────────────────────────────────────────────────────
echo ""
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
TOTAL=$((PASS + FAIL))
if [[ $FAIL -eq 0 ]]; then
    echo "${GRN}${BLD}ALL TESTS PASSED${RST}  ($PASS / $TOTAL)"
else
    echo "${RED}${BLD}TESTS FAILED${RST}  ${GRN}$PASS passed${RST}  ${RED}$FAIL failed${RST}  ($TOTAL total)"
fi
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
[[ $FAIL -eq 0 ]] && exit 0 || exit 1
