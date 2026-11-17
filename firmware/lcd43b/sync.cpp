// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega

// Refresh engine implementation: Wi-Fi, NTP, the device manifest and the
// frame downloader with bounded reads (see sync.h). Compiled into the
// sketch and into the host test (test_lcd43b_sync, with the Arduino
// fakes and AddressSanitizer watching the frame buffer).

#include "sync.h"

#ifdef VENTANAL_HOST_TEST
#include "config.example.h"
#else
#ifdef VENTANAL_LCD_CONFIG
#include VENTANAL_LCD_CONFIG
#else
#include "config.example.h"
#endif
#endif

// Optional private-config keys with the same safe defaults as the sketch.
#ifndef VENTANAL_WIFI_SSID
#define VENTANAL_WIFI_SSID "your-network-name"
#endif
#ifndef VENTANAL_WIFI_PASSWORD
#define VENTANAL_WIFI_PASSWORD "your-network-password"
#endif
#ifndef VENTANAL_HOST_URL
#define VENTANAL_HOST_URL "http://192.168.0.10:18780"
#endif
#ifndef VENTANAL_TZ
#define VENTANAL_TZ "MST7MDT,M3.2.0,M11.1.0"
#endif

#include <HTTPClient.h>
#include <WiFi.h>
#include <esp_heap_caps.h>
#include <esp_sntp.h>

#include <algorithm>

namespace lcd {

namespace {

bool wifiConnect(Watchdog& watchdog) {
    if (WiFi.status() == WL_CONNECTED) return true;
    WiFi.mode(WIFI_STA);
    WiFi.begin(VENTANAL_WIFI_SSID, VENTANAL_WIFI_PASSWORD);
    const uint32_t started = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - started < WIFI_TIMEOUT_MS) {
        watchdog.feed();
        delay(50);
    }
    const bool connected = WiFi.status() == WL_CONNECTED;
    log("wifi connected=%d\n", connected);
    return connected;
}

bool syncTime(Watchdog& watchdog) {
    configTzTime(VENTANAL_TZ, "pool.ntp.org", "time.nist.gov");
    const uint32_t started = millis();
    while (millis() - started < NTP_TIMEOUT_MS) {
        watchdog.feed();
        if (sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED) {
            log("time ntp=synced\n");
            return true;
        }
        delay(100);
    }
    log("time ntp=unavailable\n");
    return false;
}

bool fetchManifest(const char* hostUrl, const char* deviceId,
                   DeviceManifest& out, Watchdog& watchdog) {
    HTTPClient http;
    const String url = String(hostUrl) + "/devices/" + deviceId
                     + "/manifest.json";
    http.begin(url);
    http.setTimeout(HTTP_TIMEOUT_MS);
    watchdog.feed();
    const int status = http.GET();
    watchdog.feed();
    if (status != 200 || http.getSize() <= 0 || http.getSize() > 8192) {
        log("manifest status=%d\n", status);
        http.end();
        return false;
    }
    const String body = http.getString();
    http.end();
    DeviceManifest parsed;
    if (!parseDeviceManifest(body.c_str(), parsed)) {
        log("manifest parsed=0\n");
        return false;
    }
    out = parsed;
    log("manifest parsed=1 screens=%d%d%d spares=%u quiet=%04d-%04d\n",
        out.screens[0].present, out.screens[1].present, out.screens[2].present,
        out.spareCount, out.quietValid() ? out.quietStart : -1,
        out.quietValid() ? out.quietEnd : -1);
    return true;
}

// Commit a verified frame to its cache file via .tmp + rename so a crash
// mid-write can never leave a half frame under the real name.
bool commitFrame(const uint8_t* buffer, const char* slotName) {
    const String path = String("/") + slotName + ".vnr";
    const String temporary = path + ".tmp";
    File out = LittleFS.open(temporary, "w");
    if (!out) return false;
    size_t written = 0;
    while (written < FRAME_BYTES) {
        const size_t n = out.write(buffer + written, FRAME_BYTES - written);
        if (!n) break;
        written += n;
    }
    out.close();
    return written == FRAME_BYTES && LittleFS.rename(temporary, path);
}

}  // namespace

void configureTimezone() {
    // setup() runs this before the first refresh so the persisted local
    // quiet window evaluates in the configured zone even when the board
    // never reaches Wi-Fi (offline reboot with retained RTC time).
    configTzTime(VENTANAL_TZ, "pool.ntp.org", "time.nist.gov");
}

bool downloadFrame(const char* hostUrl, const FrameInfo& info,
                   const char* slotName, uint8_t* buffer, Watchdog& watchdog) {
    const String url = String(hostUrl) + info.url;
    bool ok = false;
    bool excess = false;
    HTTPClient http;
    http.begin(url);
    http.setTimeout(HTTP_TIMEOUT_MS);
    const uint32_t started = millis();
    watchdog.feed();
    const int status = http.GET();
    watchdog.feed();
    if (status == 200 && http.getSize() == int(FRAME_BYTES)) {
        size_t got = 0;
        WiFiClient* stream = http.getStreamPtr();
        while (got < FRAME_BYTES && millis() - started < TRANSFER_BUDGET_MS) {
            watchdog.feed();
            const int available = stream->available();
            if (available > 0) {
                // Every read is bounded by the bytes still missing, so the
                // PSRAM staging buffer can never overflow no matter what
                // the server sends (pinned by test_lcd43b_sync).
                const size_t remaining = FRAME_BYTES - got;
                const size_t n = std::min({size_t(available), CHUNK_BYTES,
                                          remaining});
                const int read = stream->readBytes(buffer + got, n);
                if (read <= 0) break;
                got += size_t(read);
            } else {
                if (!http.connected()) break;
                delay(2);
            }
        }
        // Connection-close framing with a bounded completion check: the
        // frame is only committed after the server hangs up with exactly
        // the advertised bytes. Trailing bytes reject the download
        // whenever they arrive — a delayed packet after a hash-matching
        // frame is malformed, not verified — and a connection that
        // neither delivers nor closes within the grace window is
        // incomplete: fail closed, keep the previous cache entry.
        bool closed = false;
        if (got == FRAME_BYTES) {
            const uint32_t closeStarted = millis();
            while (true) {
                watchdog.feed();
                if (stream->available() > 0) {
                    excess = true;
                    break;
                }
                if (!http.connected()) {
                    closed = true;
                    break;
                }
                if (millis() - closeStarted >= FRAME_CLOSE_GRACE_MS) break;
                delay(2);
            }
        }
        ok = closed && !excess
          && frameVerified(buffer, FRAME_BYTES, info.sha256);
        log("download slot=%s id=%.16s bytes=%u verified=%d excess=%d"
            " ms=%lu\n",
            slotName, info.id, unsigned(got), ok, excess,
            static_cast<unsigned long>(millis() - started));
    } else {
        log("download slot=%s status=%d size=%d\n", slotName, status,
            http.getSize());
    }
    http.end();
    if (ok) ok = commitFrame(buffer, slotName);
    // A failed or committed download must never leave a staging file.
    LittleFS.remove(String("/") + slotName + ".vnr.tmp");
    return ok;
}

void runRefresh(const char* hostUrl, const char* deviceId,
                const char currentIds[SCREEN_SLOTS][17],
                RefreshResult& result, Watchdog& watchdog) {
    result = RefreshResult{};
    result.wifiOk = wifiConnect(watchdog);
    if (!result.wifiOk) return;
    result.timeOk = syncTime(watchdog);
    if (!fetchManifest(hostUrl, deviceId, result.manifest, watchdog)) return;
    result.manifestOk = true;

    // One pass over every cache slot: download when the published id
    // changed, skip when it did not, stage a drop when the screen is
    // absent. Failures keep the previous cache entry (staged as neither).
    const FrameInfo* published[SCREEN_SLOTS] = {};
    published[0] = &result.manifest.screens[0];
    published[1] = &result.manifest.screens[1];
    published[2] = &result.manifest.screens[2];
    for (int i = 0; i < result.manifest.spareCount; ++i)
        published[3 + i] = &result.manifest.spares[i];

    uint8_t* buffer = static_cast<uint8_t*>(ps_malloc(FRAME_BYTES));
    if (!buffer) {
        log("download error=allocation\n");
        return;
    }
    for (int slot = 0; slot < SCREEN_SLOTS; ++slot) {
        watchdog.feed();
        const FrameInfo* info = published[slot];
        if (!info || !info->present) {
            // Dropped screens: erase the cached frame HERE, on the worker —
            // a ~750 KB LittleFS erase takes over a second of flash time
            // and must never stall the touch poll in loop().
            result.dropSlot[slot] = true;
            const String path = String("/") + SLOT_NAMES[slot] + ".vnr";
            const String temporary = path + ".tmp";
            if (LittleFS.exists(path)) {
                LittleFS.remove(path);
                log("cache removed slot=%s\n", SLOT_NAMES[slot]);
            }
            LittleFS.remove(temporary);
            continue;
        }
        if (!strcmp(currentIds[slot], info->id)
            && LittleFS.exists(String("/") + SLOT_NAMES[slot] + ".vnr")) {
            log("download slot=%s skipped=unchanged\n", SLOT_NAMES[slot]);
            strlcpy(result.frameIds[slot], info->id,
                    sizeof result.frameIds[slot]);
            result.frameOk[slot] = true;    // keep the current entry
            continue;
        }
        if (downloadFrame(hostUrl, *info, SLOT_NAMES[slot], buffer, watchdog)) {
            strlcpy(result.frameIds[slot], info->id,
                    sizeof result.frameIds[slot]);
            result.frameOk[slot] = true;
        }
    }
    free(buffer);
}

}  // namespace lcd
