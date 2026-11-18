// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega

// Refresh engine implementation: Wi-Fi, NTP, the device manifest and the
// frame downloader with bounded reads (see sync.h). Compiled into the
// sketch and into the host test (test_lcd43b_sync, with the Arduino
// fakes and AddressSanitizer watching the frame buffer).

#include "sync.h"
#include "../common/release_key.h"
#include "inflate.h"
#include "scanout_diag.h"
#include <vector>

#include "host_request.h"
#include <esp_heap_caps.h>
#include <esp_sntp.h>

#include <algorithm>

namespace lcd {

namespace {

bool wifiConnect(Watchdog& watchdog) {
    if (WiFi.status() == WL_CONNECTED) return true;
    WiFi.mode(WIFI_STA);
    WiFi.begin(installation.ssid, installation.password);
    const uint32_t started = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - started < WIFI_TIMEOUT_MS) {
        watchdog.feed();
        delay(50);
    }
    const bool connected = WiFi.status() == WL_CONNECTED;
    if (connected) log("wifi connected=1 rssi_dbm=%d\n", int(WiFi.RSSI()));
    else log("wifi connected=0\n");
    return connected;
}

bool syncTime(Watchdog& watchdog) {
    configTzTime(installation.timezone, "pool.ntp.org", "time.nist.gov");
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
                   RefreshResult& result, Watchdog& watchdog, const char* skips,
                   const char* otaReport, const char* otaApproval, ScanHealth health) {
    HostRequest request;
    auto& http = request.http;
    // Skip ids are 16 lowercase hex digits (including restored owner state).
    // The comma and tilde separators are permitted literally in a query.
    String path = String("/devices/") + deviceId + "/manifest.json?board=" + BOARD_ID
                  + "&fw=" + FIRMWARE_VERSION;
    if (VENTANAL_TEST_RELEASE_KEY) path += "&test_release_key=1";
    char counters[48];
    snprintf(counters, sizeof counters, "%lu,%lu,%lu,%lu",
             static_cast<unsigned long>(health.lateRestarts),
             static_cast<unsigned long>(health.shortFrames),
             static_cast<unsigned long>(health.maxFillUs),
             static_cast<unsigned long>(health.maxVsyncGapUs));
    path += String("&lcd_diag=") + counters;
    if (skips[0]) path += String("&skips=") + skips;
    if (otaReport[0]) path += String("&ota=") + otaReport;
    if (otaApproval[0]) path += String("&ota_approval=") + otaApproval;
    const auto key = decodeDeviceKey(installation.deviceKey);
    if (!request.begin(hostUrl, path, key)) {
        log("manifest transport=unavailable\n");
        return false;
    }
    watchdog.feed();
    const int status = http.GET();
    watchdog.feed();
    if (status != 200 || http.getSize() <= 0 || http.getSize() > 6144) {
        log("manifest status=%d\n", status);
        return false;
    }
    const String body = http.getString();
    const bool secured = !key.empty();
    JsonDocument doc;
    const bool ok = body.length() == unsigned(http.getSize())
        && !deserializeJson(doc, body)
        && (!secured || verifyManifestSignature(doc, key))
        && parseDeviceManifest(doc.as<JsonVariantConst>(), result.manifest);
    if (!ok) {
        log("manifest parsed=0 secured=%d\n", secured);
        return false;
    }
    parseFirmwareOffer(doc["firmware"], result.firmware);
    const auto& out = result.manifest;
    log("manifest parsed=1 screens=%d%d%d spares=%u quiet=%04d-%04d secured=%d\n",
        out.screens[0].present, out.screens[1].present, out.screens[2].present,
        out.spareCount, out.quietValid() ? out.quietStart : -1,
        out.quietValid() ? out.quietEnd : -1, secured);
    return true;
}

}  // namespace

// Both writers run on the main loop while rendering is idle. Rename only
// after a complete write, keeping the old frame on an I/O failure.
template<class Write>
bool commitFrame(const char* path, Write write) {
    ScanScope activity(ScanActivity::LittleFS);
    const String temporary = String(path) + ".tmp";
    File out = LittleFS.open(temporary, "w");
    const bool complete = out && write(out);
    out.close();
    const bool ok = complete && LittleFS.rename(temporary.c_str(), path);
    if (!ok) LittleFS.remove(temporary);
    return ok;
}

static bool writeAll(File& out, const uint8_t* bytes, size_t size) {
    while (size) {
        const size_t n = out.write(bytes, size);
        if (!n) return false;
        bytes += n;
        size -= n;
    }
    return true;
}

bool writeCachedFrame(const char* path, const FrameBody& body) {
    return commitFrame(path, [&](File& out) {
        return writeAll(out, body.bytes.get(), body.size);
    });
}

bool copyCachedFrame(const char* path, const char* source) {
    ScanScope activity(ScanActivity::LittleFS);
    File in = LittleFS.open(source, "r");
    if (!in || in.size() < HEADER_BYTES || in.size() > FRAME_WIRE_MAX) return false;
    return commitFrame(path, [&](File& out) {
        uint8_t chunk[1024];
        for (size_t remaining = in.size(); remaining;) {
            const size_t size = std::min(sizeof chunk, remaining);
            if (in.read(chunk, size) != size || !writeAll(out, chunk, size)) return false;
            remaining -= size;
        }
        return true;
    });
}

String residentPath(int slot, bool alternate) {
    return String("/") + SLOT_NAMES[slot] + (alternate ? "-alt.vnr" : ".vnr");
}

static bool removeCacheFile(const String& path) {
    ScanScope activity(ScanActivity::LittleFS);
    const bool removed = LittleFS.remove(path);
    log("cache cleanup path=%s removed=%d\n", path.c_str(), removed);
    return removed;
}

bool persistCacheSlot(Preferences& prefs, int slot, CacheLocation& current,
                      const DeferredSave& save) {
    CacheLocation next;
    if (save.id[0]) {
        memcpy(next.id, save.id, sizeof next.id);
        next.alternate = !current.alternate;
        const String path = residentPath(slot, next.alternate);
        if (!save.body.bytes || !writeCachedFrame(path.c_str(), save.body)) return false;
    }
    if (!saveCacheLocation(prefs, slot, next)) return false;
    current = next;
    for (const bool alternate : {false, true}) {
        if (next.id[0] && alternate == next.alternate) continue;
        const String path = residentPath(slot, alternate);
        if (LittleFS.exists(path)) removeCacheFile(path);
    }
    return true;
}

void sweepCacheTemps() {
    // Cache writers put their temporary files in the root. Close the
    // directory before deleting entries so enumeration is stable.
    File root = LittleFS.open("/", "r");
    if (!root || !root.isDirectory()) {
        log("cache sweep error=open-root\n");
        return;
    }
    std::vector<String> paths;
    for (File file = root.openNextFile(); file; file = root.openNextFile()) {
        const char* path = file.path();
        const size_t size = strlen(path);
        if (!file.isDirectory() && size >= 4 && strcmp(path + size - 4, ".tmp") == 0)
            paths.emplace_back(path);
        file.close();
    }
    root.close();
    unsigned removed = 0;
    for (const String& path : paths) removed += removeCacheFile(path);
    log("cache sweep tmp=%u removed=%u\n", unsigned(paths.size()), removed);
}

void configureTimezone() {
    // setup() runs this before the first refresh so the persisted local
    // quiet window evaluates in the configured zone even when the board
    // never reaches Wi-Fi (offline reboot with retained RTC time).
    configTzTime(installation.timezone, "pool.ntp.org", "time.nist.gov");
}

bool connectForUpdates(Watchdog& watchdog) {
    return wifiConnect(watchdog) && (time(nullptr) >= 1704067200 || syncTime(watchdog));
}

static bool receiveAttempt(const char* hostUrl, const FrameInfo& info,
                           const char* slotName, uint8_t* buffer, Watchdog& watchdog,
                           uint32_t started, bool& retryable) {
    bool ok = false;
    bool excess = false;
    HostRequest request;
    auto& http = request.http;
    if (!request.begin(hostUrl, String(info.url), decodeDeviceKey(installation.deviceKey)))
        return false;
    const uint32_t attemptStarted = millis();
    watchdog.feed();
    const int status = http.GET();
    watchdog.feed();
#ifdef VENTANAL_LCD_DIAG_PANEL
    constexpr auto internal = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    log("panel heap free_internal=%u largest_internal=%u\n",
        unsigned(heap_caps_get_free_size(internal)),
        unsigned(heap_caps_get_largest_free_block(internal)));
#endif
    if (status == 200 && http.getSize() == int(info.size)) {
        size_t got = 0;
        uint32_t lastProgress = millis();
        bool idle = false;
        WiFiClient* stream = http.getStreamPtr();
        while (stream && got < info.size && millis() - started < TRANSFER_BUDGET_MS) {
            watchdog.feed();
            const int available = stream->available();
            if (available > 0) {
                // Every read is bounded by the bytes still missing, so the
                // PSRAM staging buffer can never overflow no matter what
                // the server sends (pinned by test_lcd43b_sync).
                const size_t remaining = info.size - got;
                const size_t n = std::min({size_t(available), CHUNK_BYTES,
                                          remaining});
                const int read = stream->read(buffer + got, n);
                if (read < 0) break;
                if (read > 0) {
                    got += size_t(read);
                    lastProgress = millis();
                    continue;
                }
            }
            if (!http.connected()) break;
            if (millis() - lastProgress >= BODY_IDLE_MS) {
                idle = true;
                break;
            }
            delay(2);
        }
        // Connection-close framing with a bounded completion check: the
        // frame is only accepted after the server hangs up with exactly
        // the advertised bytes. Trailing bytes reject the download
        // whenever they arrive — a delayed packet after a hash-matching
        // frame is malformed, not verified — and a connection that
        // neither delivers nor closes within the grace window is
        // incomplete: fail closed, keep the previous cache entry.
        bool closed = false;
        if (got == info.size) {
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
          && strncmp(info.id, info.sha256, 16) == 0
          && frameHashMatches(buffer, info.size, info.sha256, 64);
        retryable = got < info.size;
        const char* outcome = ok ? "verified" : excess ? "excess"
            : idle ? "body-idle"
            : retryable ? (millis() - started >= TRANSFER_BUDGET_MS
                                       ? "body-timeout" : "short-body")
            : closed ? "invalid-frame" : "close-timeout";
        log("download slot=%s id=%.16s bytes=%u verified=%d excess=%d"
            " ms=%lu outcome=%s\n",
            slotName, info.id, unsigned(got), ok, excess,
            static_cast<unsigned long>(millis() - attemptStarted), outcome);
    } else {
        log("download slot=%s status=%d size=%d\n", slotName, status,
            http.getSize());
    }
    return ok;
}

bool receiveFrame(const char* hostUrl, const FrameInfo& info,
                         const char* slotName, uint8_t* buffer, Watchdog& watchdog) {
    if (!buffer || !validWireSize(info.encoding, info.size)) return false;
    const uint32_t started = millis();
    bool retryable = false;
    if (receiveAttempt(hostUrl, info, slotName, buffer, watchdog, started, retryable))
        return true;
    // A partial or stalled body is a transport failure. Retry once with
    // a fresh connection, retaining the original frame's overall budget.
    if (!retryable || millis() - started >= TRANSFER_BUDGET_MS - 2 * HTTP_TIMEOUT_MS)
        return false;
    log("download slot=%s retry=body\n", slotName);
    return receiveAttempt(hostUrl, info, slotName, buffer, watchdog, started, retryable);
}

void runRefresh(const char* hostUrl, const char* deviceId,
                const char currentIds[SCREEN_SLOTS][17],
                RefreshResult& result, Watchdog& watchdog,
                const DeviceSettings& settings, const char* skips,
                const ResidentCache& cache, const char* otaReport, const char* otaApproval) {
    result = RefreshResult{};
    result.wifiOk = wifiConnect(watchdog);
    if (!result.wifiOk) return;
    DownloadPower radio;
    result.timeOk = syncTime(watchdog);
    if (!fetchManifest(hostUrl, deviceId, result, watchdog, skips, otaReport, otaApproval, cache.health)) return;
    result.manifestOk = true;
    if (cache.manifestVerified) cache.manifestVerified();

    // One pass over every cache slot: download when the published id
    // changed, skip when it did not, stage a drop when the screen is
    // absent. Failures keep the previous cache entry (staged as neither).
    const FrameInfo* published[SCREEN_SLOTS] = {};
    published[0] = &result.manifest.screens[0];
    published[1] = &result.manifest.screens[1];
    published[2] = &result.manifest.screens[2];
    for (int i = 0; i < result.manifest.spareCount; ++i)
        published[3 + i] = &result.manifest.spares[i];

    bool visible[SCREEN_SLOTS]={}; int slots[SCREEN_SLOTS];
    const int count=activeSlots(result.manifest.screens[2].present,result.manifest.spareCount,
                                settings,slots,SCREEN_SLOTS);
    for(int i=0;i<count;++i) visible[slots[i]]=true;
    for(int i=0;i<result.manifest.spareCount;++i) visible[3+i]=settings.showArt;
    // Get the small, frequently updated pages onto the display before art.
    for (int slot : {1, 2, 0, 3, 4, 5}) {
        watchdog.feed();
        const FrameInfo* info = published[slot];
        if (!info || !info->present) {
            result.dropSlot[slot] = true;
            if (currentIds[slot][0]) cache.publish(slot, nullptr, {});
            continue;
        }
        if (!visible[slot]) {
            log("download slot=%s skipped=hidden\n", SLOT_NAMES[slot]);
            continue; // keep hidden cached frames and ids
        }
        bool force = cache.forceDownloads;
#ifdef VENTANAL_LCD_DIAG_PANEL
        force |= slot == 1; // sample weather transport each diagnostic refresh
#endif
        const bool unchanged = !force && !strcmp(currentIds[slot], info->id);
        if (unchanged) {
            log("download slot=%s skipped=unchanged\n", SLOT_NAMES[slot]);
            strlcpy(result.frameIds[slot], info->id,
                    sizeof result.frameIds[slot]);
            result.frameOk[slot] = true;    // keep the current entry
            continue;
        }
        uint8_t* bytes = nullptr;
        if (cache.staging && cache.allocated[slot]) {
            bytes = static_cast<uint8_t*>(ps_malloc(info->size));
            if (!bytes && cache.reclaimWireMemory) {
                cache.reclaimWireMemory(info->size);
                bytes = static_cast<uint8_t*>(ps_malloc(info->size));
            }
        }
        if (!bytes) {
            log("download slot=%s skipped=no-psram bytes=%u\n", SLOT_NAMES[slot],
                unsigned(info->size));
            result.frameFailed[slot] = true;
            continue;
        }
        FrameBody body{std::shared_ptr<uint8_t>(bytes, free), info->size};
        const bool received = [&] {
            ScanScope activity(ScanActivity::Receive);
            return receiveFrame(hostUrl, *info, SLOT_NAMES[slot], bytes, watchdog);
        }();
        const uint32_t decodeStart = millis();
        const bool decoded = received && [&] {
            ScanScope activity(ScanActivity::Inflate);
            return decodeFrame(info->encoding, bytes, info->size, cache.staging);
        }();
        if (received)
            log("inflate slot=%s bytes=%u ok=%d ms=%lu\n", SLOT_NAMES[slot],
                unsigned(info->size), decoded, static_cast<unsigned long>(millis() - decodeStart));
        if (decoded && cache.publish(slot, info, body)) {
            strlcpy(result.frameIds[slot], info->id,
                    sizeof result.frameIds[slot]);
            result.frameOk[slot] = true;
        } else {
            // This slot's download was required (id changed or cache
            // missing) and it failed: track it separately from manifest
            // success so the pass can never report as fully current.
            result.frameFailed[slot] = true;
        }
    }
}

}  // namespace lcd
