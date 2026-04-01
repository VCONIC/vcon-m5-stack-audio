# Captive Portal WiFi Provisioning — Implementation Plan

## Context

Device is an M5Stack Core2 (ESP32, Arduino framework, M5Unified library).
WiFi credentials and POST URL are currently stored in NVS flash via the
`Preferences` library. The serial `wifi` command already writes to NVS.
All new code must be non-breaking — the serial command interface and OTA
logic remain untouched.

No new libraries are required. `DNSServer` and `WebServer` both ship with
the `esp32:esp32` Arduino core that is already installed.

---

## Goal

On first boot (no SSID stored in NVS), instead of attempting a normal WiFi
connection that will time out, the device enters provisioning mode:

1. Broadcasts a WiFi AP named `vCon-Setup` (no password).
2. Runs a DNS server that answers all queries with 192.168.4.1, causing
   phones to auto-open a browser (the captive portal trigger).
3. Serves a single-page config form at http://192.168.4.1 over a tiny
   HTTP server.
4. On form submit, saves SSID, password, and optionally POST URL to NVS,
   then reboots into normal operation.

On all subsequent boots the stored SSID is present so the portal is skipped
entirely.

The portal can also be re-entered at any time by holding Button C at boot
(for re-provisioning at a new site).

---

## Files to create

### `portal.h`

A single self-contained header in the sketch directory alongside `config.h`.
All portal logic lives here so the main `.ino` file stays clean.

Declare and implement the following:

```cpp
#ifndef PORTAL_H
#define PORTAL_H

#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <M5Unified.h>
#include "config.h"

// Call from setup() when no credentials are found.
// Blocks until the user submits valid credentials, then saves to NVS.
// Device must be rebooted after this returns (call ESP.restart()).
void runCaptivePortal(Preferences& prefs);

#endif
```

The implementation inside `portal.h` (or a companion `portal.cpp` if the
agent prefers a .cpp split) must do the following in order:

**Step 1 — Draw the setup screen on the M5Core2 display**

Show a clear instruction screen before the AP is even started.
Use M5.Display — approximate layout:

```
+---------------------------------+
|         vCon Setup              |
|                                 |
|  1. On your phone, connect to:  |
|     WiFi: vCon-Setup            |
|                                 |
|  2. A setup page will open.     |
|     If it doesn't, open:        |
|     http://192.168.4.1          |
|                                 |
|  Waiting for configuration...   |
+---------------------------------+
```

Use a large, readable font. White text on a dark background.
Do not use any panel layout from the main UI — this is a full-screen modal.

**Step 2 — Start AP mode**

```cpp
WiFi.mode(WIFI_AP);
WiFi.softAP("vCon-Setup");  // no password — open network
delay(100);                  // allow AP stack to settle
```

AP IP will be 192.168.4.1 automatically.

**Step 3 — Start DNS server**

```cpp
DNSServer dns;
dns.start(53, "*", IPAddress(192, 168, 4, 1));
```

This catches all DNS queries and redirects them to the device, which is what
causes iOS and Android to detect a captive portal and pop up the browser.

**Step 4 — Build and start the HTTP server**

```cpp
WebServer server(80);
```

Register the following routes:

`GET /`
Serve the provisioning HTML page (see HTML spec below).

`GET /generate_204` and `GET /204`
Return HTTP 204 with no body. Required for Android captive portal detection.

`GET /hotspot-detect.html` and `GET /library/test/success.html`
Return a minimal HTML page with the word `Success` in the body.
Required for Apple (iOS/macOS) captive portal detection.

`POST /save`
Handle form submission (see below).

`GET` catch-all (`server.onNotFound`)
Redirect any unrecognised path to `http://192.168.4.1/` with HTTP 302.
This is essential — many devices probe random paths before showing the portal.

**Step 5 — The provisioning HTML page**

The HTML served at `GET /` must be a complete self-contained page (no
external resources — the phone has no internet at this point).

Required fields:

| Field name | Type | Label |
|---|---|---|
| `ssid` | text | WiFi Network Name |
| `password` | password | WiFi Password |
| `url` | text | Recorder Endpoint URL |

Pre-populate the `url` field with `DEFAULT_POST_URL` from `config.h`.

The form must POST to `/save`.

Keep the HTML minimal but legible on a phone screen. A centered card layout
with large tap targets is sufficient. Inline CSS only, no frameworks.

Add a note below the form: "Leave Endpoint URL unchanged unless instructed."

**Step 6 — Handle POST /save**

```cpp
server.on("/save", HTTP_POST, [&]() {
    String ssid     = server.arg("ssid");
    String password = server.arg("password");
    String url      = server.arg("url");

    if (ssid.isEmpty()) {
        server.send(400, "text/plain", "SSID is required");
        return;
    }

    prefs.begin("vcon", false);
    prefs.putString("ssid",     ssid);
    prefs.putString("password", password);
    if (!url.isEmpty()) prefs.putString("postUrl", url);
    prefs.end();

    // Confirm to the user before rebooting
    server.send(200, "text/html",
        "<html><body style='font-family:sans-serif;text-align:center;padding:40px'>"
        "<h2>Saved!</h2>"
        "<p>The recorder will now connect to <strong>" + ssid + "</strong>.</p>"
        "<p>You can reconnect to your normal WiFi network.</p>"
        "</body></html>");

    delay(2000);
    ESP.restart();
});
```

**Step 7 — The blocking event loop**

```cpp
server.begin();
while (true) {
    dns.processNextRequest();
    server.handleClient();
    delay(5);
}
```

This loop never exits — the restart in the POST handler is the only way out.

---

## Files to modify

### `VConRecorder.ino` — boot sequence changes only

**Add at the top:**
```cpp
#include "portal.h"
```

**Add a helper function `hasStoredCredentials()`:**
```cpp
bool hasStoredCredentials() {
    Preferences p;
    p.begin("vcon", true);
    String ssid = p.getString("ssid", "");
    p.end();
    return ssid.length() > 0;
}
```

**Modify `setup()` — add portal trigger before `connectWiFi()`:**

Replace or wrap the existing `connectWiFi()` call with:

```cpp
// Check for forced re-provisioning (hold Button C at boot)
M5.update();
bool forcePortal = M5.BtnC.isPressed();

if (forcePortal || !hasStoredCredentials()) {
    runCaptivePortal(prefs);  // blocks until saved, then reboots
    // never reaches here
}

connectWiFi();  // existing call — unchanged
```

The `prefs` object passed to `runCaptivePortal` must be the same
`Preferences` instance used by `loadConfig()` / `saveConfig()` in the
existing code. If `prefs` is declared locally in `setup()`, pass it by
reference. If it is global, pass the global.

**No other changes to the main sketch.**

The existing `connectWiFi()`, `loadConfig()`, `saveConfig()`,
`handleSerialCommand()`, OTA check, and all recording logic are untouched.

---

## config.h changes

Add one define alongside the existing WiFi defaults:

```cpp
// AP name broadcast during captive portal provisioning
#define PORTAL_AP_NAME  "vCon-Setup"
```

Use `PORTAL_AP_NAME` in `portal.h` instead of the hardcoded string.

---

## NVS key compatibility

The portal must use the same NVS namespace and key names as the existing
`loadConfig()` / `saveConfig()` functions. Check the main sketch for the
exact strings — they are likely `"vcon"` / `"ssid"` / `"password"` /
`"postUrl"`. The portal and the serial `wifi` command must write to identical
keys so either path produces the same result.

---

## Button C re-provisioning

Holding Button C during the first second of boot triggers the portal even
when credentials are already stored. This is the recovery path for moving
the device to a new network without a serial cable.

The display should indicate this on the normal boot splash if it exists,
e.g. "Hold C to re-configure WiFi".

---

## Testing checklist

- [ ] Fresh device (NVS erased with `esptool.py erase_flash`) boots into
      portal mode automatically
- [ ] `vCon-Setup` network is visible on an iPhone and Android phone
- [ ] iOS auto-opens the portal page (captive portal detection fires)
- [ ] Android auto-opens the portal page (generate_204 probe is answered)
- [ ] Form submits, device reboots, connects to the target network
- [ ] On next boot, portal is skipped and normal operation resumes
- [ ] Serial `wifi` command still works after portal provisioning
- [ ] Button C at boot re-enters portal on a provisioned device
- [ ] Endpoint URL field is pre-populated with the default value
- [ ] Submitting with an empty SSID returns an error, not a crash

---

## What does not change

- Serial command interface (`wifi`, `url`, `status`, `restart`, `help`, `scan`, `sd`)
- OTA check logic
- vCon recording, encoding, and upload logic
- SD card handling
- Display layout during normal operation
- NVS key names (portal writes to the same keys as the serial command)
