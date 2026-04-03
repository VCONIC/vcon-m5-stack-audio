#ifndef OTA_H
#define OTA_H

// =============================================================================
// OTA firmware update — HTTP polling, ESP32 built-in Update library
// =============================================================================
// Call checkForOTA() once in setup(), after WiFi is confirmed connected.
// Blocks until the check (and optional download) is complete.
// If a new firmware is applied the device reboots; this function never returns.
//
// Server contract:
//   GET OTA_VERSION_URL  → plain text "X.Y.Z\n"  (must match FIRMWARE_VERSION)
//   GET OTA_FIRMWARE_URL → raw Arduino .bin, Content-Length required
//
// Partition scheme: the sketch must be built with an OTA-capable scheme
//   (e.g. "Minimal SPIFFS (1.9MB APP with OTA)" for large sketches).
// =============================================================================

#include <HTTPClient.h>
#include <Update.h>
#include "config.h"

inline void checkForOTA(Print& log) {
    HTTPClient http;

    // -------------------------------------------------------------------------
    // 1. Fetch remote version string
    // -------------------------------------------------------------------------
    http.setTimeout(10000);
    http.begin(OTA_VERSION_URL);
    int code = http.GET();
    if (code != 200) {
        log.printf("[OTA] version check failed: HTTP %d\n", code);
        http.end();
        return;
    }
    String remote = http.getString();
    remote.trim();
    http.end();

    // Server may return plain "X.Y.Z" or JSON {"version":"X.Y.Z"}.
    // Extract just the version token so the comparison is format-agnostic.
    if (remote.startsWith("{")) {
        int s = remote.indexOf('"', remote.indexOf(':') + 1);
        int e = remote.indexOf('"', s + 1);
        if (s >= 0 && e > s) remote = remote.substring(s + 1, e);
        remote.trim();
    }

    log.printf("[OTA] local=%s  remote=%s\n", FIRMWARE_VERSION, remote.c_str());

    if (remote == FIRMWARE_VERSION) {
        log.println("[OTA] firmware is current, skipping");
        return;
    }

    // -------------------------------------------------------------------------
    // 2. Download and flash new firmware
    // -------------------------------------------------------------------------
    log.printf("[OTA] update available (%s → %s), downloading...\n",
               FIRMWARE_VERSION, remote.c_str());

    http.setTimeout(60000);   // large binary may take a while on slow WiFi
    http.begin(OTA_FIRMWARE_URL);
    code = http.GET();
    if (code != 200) {
        log.printf("[OTA] firmware download failed: HTTP %d\n", code);
        http.end();
        return;
    }

    int contentLen = http.getSize();
    if (contentLen <= 0) {
        log.println("[OTA] server did not send Content-Length, aborting");
        http.end();
        return;
    }
    log.printf("[OTA] binary size: %d bytes\n", contentLen);

    WiFiClient* stream = http.getStreamPtr();

    if (!Update.begin(contentLen)) {
        log.printf("[OTA] Update.begin failed (error %d) — wrong partition scheme?\n",
                   Update.getError());
        http.end();
        return;
    }

    // Stream the binary into flash, printing progress every ~10%
    size_t written   = 0;
    size_t lastPrint = 0;
    uint8_t buf[512];
    while (written < (size_t)contentLen) {
        int avail = stream->available();
        if (avail <= 0) { delay(1); continue; }
        int toRead = min(avail, (int)sizeof(buf));
        int got    = stream->readBytes(buf, toRead);
        if (got <= 0) break;
        Update.write(buf, got);
        written += got;
        if (written - lastPrint >= (size_t)(contentLen / 10)) {
            log.printf("[OTA] %u / %d bytes (%.0f%%)\n",
                       written, contentLen, 100.0f * written / contentLen);
            lastPrint = written;
        }
    }
    log.printf("[OTA] wrote %u / %d bytes\n", written, contentLen);

    if (!Update.end(true)) {
        log.printf("[OTA] Update.end error: %d\n", Update.getError());
        http.end();
        return;
    }

    log.println("[OTA] success — rebooting in 1 s...");
    http.end();
    delay(1000);
    ESP.restart();
}

#endif // OTA_H
