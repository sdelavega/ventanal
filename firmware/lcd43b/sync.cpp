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
#include "wifi_roam.h"
#include <vector>

#include "../common/resume/storage.h"
#include "host_transport.h"
#include <esp_heap_caps.h>
#include <esp_sntp.h>
#include <esp_wifi.h>

#include <algorithm>

namespace lcd {

namespace {

bool joinWifi(Watchdog& watchdog) {
    WiFi.setScanMethod(WIFI_ALL_CHANNEL_SCAN);
    WiFi.setSortMethod(WIFI_CONNECT_AP_BY_SIGNAL);
    WiFi.begin(installation.ssid, installation.password);
    const uint32_t started = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - started < WIFI_TIMEOUT_MS) {
        watchdog.feed();
        delay(50);
    }
    return WiFi.status() == WL_CONNECTED;
}

int strongestWifiRssi(Watchdog& watchdog) {
    constexpr uint32_t ScanTimeoutMs = 4000;
    const uint32_t started = millis();
    // Channel 0 scans every channel. Filter by SSID in the driver.
    int count = WiFi.scanNetworks(true, true, false, 120, 0, installation.ssid);
    while (count == WIFI_SCAN_RUNNING && millis() - started < ScanTimeoutMs) {
        watchdog.feed();
        delay(25);
        count = WiFi.scanComplete();
    }
    if (count == WIFI_SCAN_RUNNING) esp_wifi_scan_stop();
    int best = -127;
    for (int i = 0; i < count; ++i) best = std::max(best, int(WiFi.RSSI(i)));
    WiFi.scanDelete();
    return best;
}

bool wifiConnect(Watchdog& watchdog) {
    bool connected = WiFi.status() == WL_CONNECTED;
    if (connected) {
        const int current = WiFi.RSSI();
        if (current < WifiRoamThresholdDbm) {
            const int best = strongestWifiRssi(watchdog);
            connected = WiFi.status() == WL_CONNECTED;
            if (connected && shouldRoam(current, best)) {
                const bool disconnected = WiFi.disconnect(false);
                connected = disconnected && joinWifi(watchdog);
                log("wifi roam from_dbm=%d to_dbm=%d ok=%d\n", current, best, int(connected));
                if (disconnected && !connected) WiFi.disconnect(false);
            }
        }
    }
    if (!connected) {
        WiFi.mode(WIFI_STA);
        connected = joinWifi(watchdog);
    }
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
        next.frameEpoch = save.frameEpoch;
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

namespace {

void logResume(const char* format, ...) {
    char line[128];
    va_list args;
    va_start(args, format);
    vsnprintf(line, sizeof line, format, args);
    va_end(args);
    log("%s\n", line);
}

void logFrameOutcome(const char* slot, size_t total, const FrameDownload& download) {
    const char* outcome = "failed";
    switch (download.outcome) {
        case FetchOutcome::Complete: outcome = "complete"; break;
        case FetchOutcome::Progress: outcome = "progress"; break;
        case FetchOutcome::NoProgress: outcome = "no-progress"; break;
        case FetchOutcome::Failed: break;
    }
    char status[24];
    const char* reason = download.reason;
    if (!reason) {
        snprintf(status, sizeof status, "status-%d", download.status);
        reason = status;
    }
    log("frame slot=%s outcome=%s reason=%s got=%u have=%u total=%u ms=%lu\n",
        slot, outcome, reason, unsigned(download.got), unsigned(download.have),
        unsigned(total), static_cast<unsigned long>(download.ms));
}

}  // namespace

FrameDownload receiveFrame(const char* hostUrl, const FrameInfo& info,
                           const char* slotName, uint8_t* buffer, Watchdog& watchdog,
                           uint32_t passStarted) {
    const uint32_t started = millis();
    const auto key = decodeDeviceKey(installation.deviceKey);
    HostRangeTransport transport(hostUrl, info.url, key, watchdog, passStarted);
    auto& store = partialStore();
    const auto target = std::string("frame-") + slotName;
    const auto result = [&](FetchOutcome outcome, const char* reason) {
        const auto size = partialStore().size(target.c_str());
        return FrameDownload{outcome, reason, transport.bytesRead(), size_t(std::max(int64_t(0), size)),
                             uint32_t(millis() - started), transport.status()};
    };
    if (!transport.remainingMs()) return result(FetchOutcome::NoProgress, "budget");
    if (!buffer) return result(FetchOutcome::Failed, "memory");
    if (!validWireSize(info.encoding, info.size)) {
        store.discard(target.c_str());
        return result(FetchOutcome::Failed, "size");
    }
    if (strncmp(info.id, info.sha256, 16) != 0) {
        store.discard(target.c_str());
        return result(FetchOutcome::Failed, "hash");
    }
    auto url = hostAssetUrl(hostUrl, info.url);
    if (url.size() >= sizeof(PartialMeta::url)) {
        // The sidecar has a fixed URL field. Preserve the full source identity
        // for longer URLs while HostRequest still uses the original URL.
        Sha256 hash;
        hash.update(reinterpret_cast<const uint8_t*>(url.data()), url.size());
        url = "sha256:" + hexBytes(hash.digest(), 32);
    }
    // The host publishes the same SHA-256 in signed and legacy manifests.
    const ResumeSource source{url.c_str(), info.id, info.sha256, uint32_t(info.size), true};
    const auto epoch = time(nullptr);
    const char* reason = nullptr;
    const auto outcome = resumeFetch(transport, store, target.c_str(), source,
        transport.remainingMs(), []() -> uint32_t { return millis(); },
        epoch >= resume_detail::kValidEpoch ? uint32_t(epoch) : 0, logResume, &reason);
    if (transport.malformed()) {
        store.discard(target.c_str());
        return result(FetchOutcome::Failed, transport.reason());
    }
    if (outcome != FetchOutcome::Complete) {
        if (&partialStore() != &store) return result(FetchOutcome::NoProgress, "storage");
        if (!reason) reason = transport.reason();
        if (!reason && (!transport.status() || transport.status() == 200 || transport.status() == 206))
            reason = "budget";
        return result(outcome, reason);
    }

    const bool loaded = store.takeCommitted(target.c_str(), buffer, info.size);
    watchdog.feed();
    if (loaded) return result(FetchOutcome::Complete, "ok");
    return result(&partialStore() != &store ? FetchOutcome::NoProgress : FetchOutcome::Failed,
                  "storage");
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
    const uint32_t passStarted = millis();
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
        if (millis() - passStarted < TRANSFER_BUDGET_MS && cache.staging && cache.allocated[slot]) {
            bytes = static_cast<uint8_t*>(ps_malloc(info->size));
            if (!bytes && cache.reclaimWireMemory) {
                cache.reclaimWireMemory(info->size);
                bytes = static_cast<uint8_t*>(ps_malloc(info->size));
            }
        }
        FrameBody body;
        if (bytes) body = FrameBody{std::shared_ptr<uint8_t>(bytes, free), info->size};
        auto download = [&] {
            ScanScope activity(ScanActivity::Receive);
            return receiveFrame(hostUrl, *info, SLOT_NAMES[slot], bytes, watchdog, passStarted);
        }();
        if (download.outcome == FetchOutcome::Complete) {
            const uint32_t decodeStart = millis();
            const bool decoded = [&] {
                ScanScope activity(ScanActivity::Inflate);
                return decodeFrame(info->encoding, bytes, info->size, cache.staging);
            }();
            log("inflate slot=%s bytes=%u ok=%d ms=%lu\n", SLOT_NAMES[slot],
                unsigned(info->size), decoded, static_cast<unsigned long>(millis() - decodeStart));
            if (!decoded || !cache.publish(slot, info, body)) {
                download.outcome = FetchOutcome::Failed;
                download.reason = decoded ? "publish" : "invalid-frame";
            }
        }
        logFrameOutcome(SLOT_NAMES[slot], info->size, download);
        if (download.outcome == FetchOutcome::Complete) {
            strlcpy(result.frameIds[slot], info->id,
                    sizeof result.frameIds[slot]);
            result.frameOk[slot] = true;
        } else {
            result.framesPending = true;
            if (download.outcome == FetchOutcome::Failed) {
                result.frameFailed[slot] = true;
            }
        }
    }
}

}  // namespace lcd
