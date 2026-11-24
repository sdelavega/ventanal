// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega

// Refresh engine implementation: Wi-Fi, NTP, the device manifest and the
// frame downloader with bounded reads (see sync.h). Compiled into the
// sketch and into the host test (test_lcd43b_sync, with the Arduino
// fakes and AddressSanitizer watching the frame buffer).

#include "sync.h"
#include "../common/release_key.h"
#include "frame_stream.h"
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

struct WifiScan {
    int count = 0, best = -127, channel = 0;
    uint8_t bssid[6]{};
    bool found = false;
};

WifiScan scanWifiOnce(Watchdog& watchdog, uint32_t budget) {
    constexpr uint32_t ScanTimeoutMs = 4000;
    const uint32_t started = millis();
    int count = WiFi.scanNetworks(true, true, false, 120, 0, installation.ssid);
    while (count == WIFI_SCAN_RUNNING && millis() - started < std::min(ScanTimeoutMs, budget)) {
        watchdog.feed(); delay(25); count = WiFi.scanComplete();
    }
    if (count == WIFI_SCAN_RUNNING) esp_wifi_scan_stop();
    WifiScan scan;
    scan.count = std::max(0, count);
    for (int i = 0; i < count; ++i) {
        const int signal = WiFi.RSSI(i);
        if (!scan.found || signal > scan.best) {
            const uint8_t* address = WiFi.BSSID(i);
            if (!address) continue;
            scan.found = true; scan.best = signal; scan.channel = WiFi.channel(i);
            memcpy(scan.bssid, address, sizeof scan.bssid);
        }
    }
    WiFi.scanDelete();
    return scan;
}

WifiScan strongestWifi(Watchdog& watchdog, uint32_t budget = UINT32_MAX) {
    const uint32_t started = millis();
    WifiScan scan = scanWifiOnce(watchdog, budget);
    constexpr uint32_t RetryDelayMs = 250;
    const uint32_t elapsed = millis() - started;
    if (scan.count == 0 && WiFi.status() == WL_CONNECTED &&
        elapsed < budget && budget - elapsed > RetryDelayMs) {
        watchdog.feed();
        delay(RetryDelayMs);
        scan = scanWifiOnce(watchdog, budget - (millis() - started));
    }
    return scan;
}

void logWifiScan(const WifiScan& scan) {
    const bool connected = WiFi.status() == WL_CONNECTED;
    log("wifi scan aps=%d best_dbm=%d best_ch=%d joined_dbm=%d joined_ch=%d\n",
        scan.count, scan.best, scan.channel, connected ? int(WiFi.RSSI()) : -127,
        connected ? int(WiFi.channel()) : 0);
}

bool joinWifi(Watchdog& watchdog, const WifiScan* previous = nullptr,
              uint32_t budget = UINT32_MAX) {
    const uint32_t started = millis();
    WiFi.setScanMethod(WIFI_ALL_CHANNEL_SCAN);
    WiFi.setSortMethod(WIFI_CONNECT_AP_BY_SIGNAL);
    const WifiScan scan = previous ? *previous : strongestWifi(watchdog, budget);
    auto remaining = [&] { const uint32_t elapsed = millis() - started;
        return elapsed < budget ? budget - elapsed : 0; };
    auto wait = [&] {
        const uint32_t begin = millis();
        while (WiFi.status() != WL_CONNECTED && millis() - begin < WIFI_TIMEOUT_MS && remaining()) {
            watchdog.feed(); delay(50);
        }
        return WiFi.status() == WL_CONNECTED;
    };
    bool connected = false;
    if (remaining() && scan.found) {
        WiFi.begin(installation.ssid, installation.password, scan.channel, scan.bssid);
        connected = wait();
        if (!connected) WiFi.disconnect(false);
    }
    if (!connected && remaining()) {
        WiFi.begin(installation.ssid, installation.password);
        connected = wait();
    }
    logWifiScan(scan);
    return connected;
}

bool wifiConnect(Watchdog& watchdog) {
    bool connected = WiFi.status() == WL_CONNECTED;
    if (connected) {
        const int current = WiFi.RSSI();
        const WifiScan scan = strongestWifi(watchdog);
        connected = WiFi.status() == WL_CONNECTED;
        if (connected && shouldRoam(current, scan.best)) {
            const bool disconnected = WiFi.disconnect(false);
            connected = disconnected && joinWifi(watchdog, &scan);
            log("wifi roam from_dbm=%d to_dbm=%d ok=%d\n", current, scan.best, int(connected));
            if (disconnected && !connected) WiFi.disconnect(false);
        } else logWifiScan(scan);
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

// The pass's first host request. Opens the pass connection; transportFailed
// says the GET itself failed (no HTTP answer), for the host-unreachable re-join.
bool fetchManifest(const char* hostUrl, const char* deviceId,
                   RefreshResult& result, Watchdog& watchdog, const char* skips,
                   const char* otaReport, const char* otaApproval, ScanHealth health,
                   HostConnection& connection, uint32_t passStarted,
                   const std::function<void(int)>& hostResult, bool& transportFailed) {
    transportFailed = false;
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
    const auto remaining = [&]() -> uint32_t {
        const uint32_t elapsed = millis() - passStarted;
        return elapsed < TRANSFER_BUDGET_MS ? TRANSFER_BUDGET_MS - elapsed : 0;
    };
    const int status = connection.get(hostUrl, path, key, remaining,
        [&](HTTPClient& http) {
            http.setConnectTimeout(std::min(remaining(), uint32_t(5000)));
            http.setTimeout(std::min(remaining(), HTTP_TIMEOUT_MS));
        },
        [&] { watchdog.feed(); });
    if (status == HostConnection::kNotBegun) {
        log("manifest transport=unavailable\n");
        return false;
    }
    hostResult(status);
    auto& http = connection.http;
    const int size = http.getSize();
    if (status != 200 || size <= 0 || size > 6144) {
        transportFailed = status <= 0;
        connection.finish(false);
        log("manifest status=%d\n", status);
        return false;
    }
    const String body = http.getString();
    // Keep the connection only after exactly Content-Length bytes.
    auto* stream = http.getStreamPtr();
    const bool exact = body.length() == unsigned(size);
    connection.finish(exact && !(stream && stream->available() > 0));
    const bool secured = !key.empty();
    JsonDocument doc;
    const bool ok = exact
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

// Host-unreachable recovery for one pass: two consecutive transport-level
// host failures (status <= 0) while Wi-Fi reports connected rescan and
// re-join the strongest AP, at most once per pass and within its budget.
class HostRecovery {
public:
    HostRecovery(Watchdog& watchdog, HostConnection& connection, uint32_t passStarted)
        : watchdog_(watchdog), connection_(connection), passStarted_(passStarted) {}

    void report(int status) {
        if (status <= 0) ++failures_;
        else failures_ = 0;
        if (failures_ >= 2) rejoin();
    }

    // True when Wi-Fi was dropped and is connected again.
    bool rejoin() {
        const uint32_t elapsed = millis() - passStarted_;
        if (rejoined_ || WiFi.status() != WL_CONNECTED || elapsed >= TRANSFER_BUDGET_MS) return false;
        rejoined_ = true;
        auto& watchdog = watchdog_;
        const int from = WiFi.RSSI();
        const uint32_t remaining = TRANSFER_BUDGET_MS - elapsed;
        const uint32_t scanStarted = millis();
        const WifiScan scan = strongestWifi(watchdog, remaining);
        const uint32_t spent = millis() - scanStarted;
        bool ok = WiFi.status() == WL_CONNECTED;
        bool disconnected = false;
        if (scan.count > 0 && scan.found && spent < remaining && ok) {
            connection_.close();   // its socket does not survive the re-join
            disconnected = WiFi.disconnect(false);
            ok = disconnected && joinWifi(watchdog, &scan, remaining - spent);
            if (disconnected && !ok) {
                const uint32_t restoreStarted = millis();
                const uint32_t restoreBudget = restoreWaitMs(passStarted_, restoreStarted,
                                                               TRANSFER_BUDGET_MS, WIFI_TIMEOUT_MS);
                if (restoreBudget) WiFi.begin(installation.ssid, installation.password);
                while (WiFi.status() != WL_CONNECTED) {
                    const uint32_t elapsed = millis() - restoreStarted;
                    if (elapsed >= restoreBudget) break;
                    watchdog.feed(); delay(std::min(uint32_t(50), restoreBudget - elapsed));
                }
                ok = WiFi.status() == WL_CONNECTED;
            }
        } else {
            logWifiScan(scan);
        }
        if (!disconnected && scan.count > 0 && scan.found && spent < remaining) logWifiScan(scan);
        log("wifi rejoin reason=host-unreachable from_dbm=%d to_dbm=%d ok=%d\n",
            from, ok ? int(WiFi.RSSI()) : -127, int(ok));
        return disconnected && ok;
    }

private:
    Watchdog& watchdog_;
    HostConnection& connection_;
    uint32_t passStarted_;
    unsigned failures_ = 0;
    bool rejoined_ = false;
};

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

static bool copyFrameBytes(File& in, File& out, FsStore* store, const char* digest, bool& readable) {
    Sha256 hash;
    uint8_t chunk[1024];
    for (size_t remaining = in.size(); remaining;) {
        const size_t size = std::min(sizeof chunk, remaining);
        if (!readFrameBytes(in, store, chunk, size)) return readable = false;
        if (digest) hash.update(chunk, size);
        if (!writeAll(out, chunk, size)) return false;
        remaining -= size;
    }
    if (digest && hexBytes(hash.digest(), 32) != digest) return readable = false;
    return true;
}

bool writeCachedFrame(const char* path, const FrameBody& body) {
    if (body.bytes) {
        return commitFrame(path, [&](File& out) {
            return writeAll(out, body.bytes.get(), body.size);
        });
    }
    if (!body.file) return false;
    auto& source = *body.file;
    auto in = source.store.openCommitted(source.target.c_str());
    bool readable = in && in.size() == body.size;
    const bool ok = readable && commitFrame(path, [&](File& out) {
        return copyFrameBytes(in, out, &source.store, source.sha256.c_str(), readable);
    });
    if (!readable) source.unavailable = true;
    return ok;
}

bool copyCachedFrame(const char* path, const char* source) {
    ScanScope activity(ScanActivity::LittleFS);
    File in = LittleFS.open(source, "r");
    if (!in || in.size() < HEADER_BYTES || in.size() > FRAME_WIRE_MAX) return false;
    bool readable = true;
    return commitFrame(path, [&](File& out) {
        return copyFrameBytes(in, out, nullptr, nullptr, readable);
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
        if (!writeCachedFrame(path.c_str(), save.body)) return false;
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

FrameDecode decodeFrameFile(FrameEncoding encoding, fs::File& file, FsStore* store,
                            uint8_t* frame, Watchdog& watchdog) {
    if (!frame) return FrameDecode::Memory;
    size_t remaining = file.size();
    if (!validWireSize(encoding, remaining)) return FrameDecode::Invalid;
    if (encoding == FrameEncoding::Jpeg) return decodeHostJpegFile(file, store, frame, watchdog);
    if (encoding == FrameEncoding::Raw) {
        for (size_t offset = 0; offset < remaining;) {
            const size_t count = std::min<size_t>(kResumeChunk, remaining - offset);
            if (!readFrameBytes(file, store, frame + offset, count)) return FrameDecode::Storage;
            offset += count;
            watchdog.feed();
        }
        return headerValid(frame) ? FrameDecode::Complete : FrameDecode::Invalid;
    }
    std::unique_ptr<uint8_t, decltype(&heap_caps_free)> chunk(
        static_cast<uint8_t*>(ps_malloc(kResumeChunk)), heap_caps_free);
    if (!chunk) return FrameDecode::Memory;
    std::unique_ptr<tinfl_decompressor, decltype(&heap_caps_free)> stream(
        static_cast<tinfl_decompressor*>(ps_malloc(sizeof(tinfl_decompressor))), heap_caps_free);
    if (!stream) return FrameDecode::Memory;
    tinfl_init(stream.get());

    size_t written = 0;
    while (remaining) {
        const size_t count = std::min<size_t>(kResumeChunk, remaining);
        if (!readFrameBytes(file, store, chunk.get(), count)) return FrameDecode::Storage;
        remaining -= count;
        size_t consumed = count;
        size_t produced = FRAME_BYTES - written;
        // Require a real zlib trailer, even for the last input block.
        constexpr auto flags = TINFL_FLAG_PARSE_ZLIB_HEADER |
            TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF | TINFL_FLAG_HAS_MORE_INPUT;
        const auto status = tinfl_decompress(stream.get(), chunk.get(), &consumed,
                                             frame, frame + written, &produced, flags);
        written += produced;
        watchdog.feed();
        if (status == TINFL_STATUS_DONE)
            return !remaining && consumed == count && written == FRAME_BYTES && headerValid(frame)
                ? FrameDecode::Complete : FrameDecode::Invalid;
        if (status != TINFL_STATUS_NEEDS_MORE_INPUT || consumed != count) break;
    }
    return FrameDecode::Invalid;
}

FrameDownload receiveFrame(const char* hostUrl, const FrameInfo& info,
                           const char* slotName, FsStore& store, Watchdog& watchdog,
                           uint32_t passStarted, const std::function<void(int)>& hostResult,
                           HostConnection* connection) {
    const uint32_t started = millis();
    const auto key = decodeDeviceKey(installation.deviceKey);
    HostRangeTransport transport(hostUrl, info.url, key, watchdog, passStarted, false, hostResult, connection);
    const auto target = std::string("frame-") + slotName;
    const auto result = [&](FetchOutcome outcome, const char* reason) {
        const auto size = store.size(target.c_str());
        return FrameDownload{outcome, reason, transport.bytesRead(), size_t(std::max(int64_t(0), size)),
                             uint32_t(millis() - started), transport.status()};
    };
    if (!transport.remainingMs()) return result(FetchOutcome::NoProgress, "budget");
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
        // for longer URLs while the request still uses the original URL.
        Sha256 hash;
        hash.update(reinterpret_cast<const uint8_t*>(url.data()), url.size());
        url = "sha256:" + hexBytes(hash.digest(), 32);
    }
    std::unique_ptr<uint8_t, decltype(&heap_caps_free)> chunk(
        static_cast<uint8_t*>(ps_malloc(kResumeChunk)), heap_caps_free);
    if (!chunk) return result(FetchOutcome::NoProgress, "memory");
    // The host publishes the same SHA-256 in signed and legacy manifests.
    const ResumeSource source{url.c_str(), info.id, info.sha256, uint32_t(info.size), true};
    const auto epoch = time(nullptr);
    const char* reason = nullptr;
    const auto outcome = resumeFetch(transport, store, target.c_str(), source,
        transport.remainingMs(), []() -> uint32_t { return millis(); },
        epoch >= resume_detail::kValidEpoch ? uint32_t(epoch) : 0, logResume, &reason,
        [&] { watchdog.feed(); }, chunk.get(), kResumeChunk);
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

    return result(FetchOutcome::Complete, "ok");
}

namespace {

// Only the refresh worker manages the leases and the names they protect.
// The loop can retain a lease for either the slot or its art override.
std::vector<std::shared_ptr<FrameFile>> frameStages;

auto findFrameStage(FsStore& store, const char* target) {
    return std::find_if(frameStages.begin(), frameStages.end(), [&](const auto& stage) {
        return &stage->store == &store && stage->target == target;
    });
}

void retireFrameStages() {
    frameStages.erase(std::remove_if(frameStages.begin(), frameStages.end(), [](const auto& stage) {
        if (stage.use_count() != 1) return false;
        stage->store.removeCommitted(stage->target.c_str());
        return true;
    }), frameStages.end());
}

std::shared_ptr<FrameFile> frameStage(FsStore& store, const std::string& target, const char* sha256) {
    const auto existing = findFrameStage(store, target.c_str());
    if (existing != frameStages.end()) return *existing;
    return std::make_shared<FrameFile>(store, target, sha256);
}

bool frameFileVerified(fs::File& file, FsStore* store, size_t size, const char* digest,
                       size_t digits, Watchdog& watchdog) {
    if (file.size() != size) return false;
    Sha256 hash;
    uint8_t block[kPartialHashBlockBytes];
    for (size_t remaining = size; remaining;) {
        const size_t count = std::min(remaining, sizeof block);
        if (!readFrameBytes(file, store, block, count)) return false;
        hash.update(block, count);
        remaining -= count;
        watchdog.feed();
    }
    return !hexBytes(hash.digest(), 32).compare(0, digits, digest);
}

// A legacy slot name is also where resumeFetch atomically commits. Move it
// to the id-qualified name before handing it to the loop, so a subsequent
// manifest can download into the same slot without overwriting a queued save.
FsStore* recoverFrameStage(const std::string& target, const std::string& version,
                           const FrameInfo* info, bool trustedClock, Watchdog& watchdog) {
    FsStore* recovered = nullptr;
    forEachPartialStore([&](FsStore& store) {
        if (recovered) return;
        const bool expected = info && info->present && validWireSize(info->encoding, info->size)
            && !strncmp(info->id, info->sha256, 16);
        for (const auto* name : {&version, &target}) {
            if (name->empty()) continue;
            auto file = store.openCommitted(name->c_str());
            if (!file) continue;
            const bool verified = expected && frameFileVerified(file, &store, info->size, info->sha256,
                                                                 64, watchdog);
            file.close();
            if (verified) {
                if (name == &version || store.moveCommitted(target.c_str(), version.c_str())) {
                    recovered = &store;
                    return;
                }
            } else if (trustedClock && !queuedFrameStage(store, name->c_str())) {
                store.removeCommitted(name->c_str());
            }
        }
    });
    return recovered;
}

}  // namespace

bool queuedFrameStage(FsStore& store, const char* target) {
    return findFrameStage(store, target) != frameStages.end();
}

bool verifyFrameFile(fs::File& file, const char* id, Watchdog& watchdog) {
    return frameFileVerified(file, nullptr, file.size(), id, 16, watchdog) && file.seek(0);
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
    // One keep-alive host connection carries the manifest and every frame
    // range of this pass; the transfer budget and the host-failure count
    // start with the manifest.
    HostConnection connection;
    const uint32_t passStarted = millis();
    HostRecovery recovery(watchdog, connection, passStarted);
    const std::function<void(int)> hostResult = [&](int status) { recovery.report(status); };
    bool manifestTransportFailed = false;
    const auto manifest = [&] {
        return fetchManifest(hostUrl, deviceId, result, watchdog, skips, otaReport, otaApproval,
                             cache.health, connection, passStarted, hostResult, manifestTransportFailed);
    };
    if (!manifest()) {
        // Nothing else in the pass can reach the host without a manifest, so
        // its transport failure completes the count: re-join now (once) and
        // fetch the manifest once more.
        if (!manifestTransportFailed || !recovery.rejoin() ||
            millis() - passStarted >= TRANSFER_BUDGET_MS || !manifest()) return;
    }
    result.manifestOk = true;
    if (cache.manifestVerified) cache.manifestVerified();
    retireFrameStages();

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
    bool otherPending = false;
    const bool trustedClock = time(nullptr) >= resume_detail::kValidEpoch;
    for (int slot : {1, 2, 0, 3, 4, 5}) {
        watchdog.feed();
        const FrameInfo* info = published[slot];
        const auto target = std::string("frame-") + SLOT_NAMES[slot];
        const auto version = info && info->present ? target + "." + info->id : std::string{};
        auto* recovered = recoverFrameStage(target, version, info, trustedClock, watchdog);
        if (!info || !info->present) {
            result.dropSlot[slot] = true;
            if (currentIds[slot][0]) cache.publish(slot, nullptr, {});
            continue;
        }
        if (!visible[slot] && !recovered && !cache.retryFileSave[slot]) {
            log("download slot=%s skipped=hidden\n", SLOT_NAMES[slot]);
            continue; // keep hidden cached frames and ids
        }
        bool force = cache.forceDownloads;
#ifdef VENTANAL_LCD_DIAG_PANEL
        force |= slot == 1; // sample weather transport each diagnostic refresh
#endif
        const bool unchanged = !force && !cache.retryFileSave[slot] && !strcmp(currentIds[slot], info->id);
        if (unchanged) {
            // Publication queues a deferred save. Keep the recovery copy
            // until the loop has committed the normal cache entry.
            if (recovered && !strcmp(cache.persistedIds[slot], info->id) &&
                !queuedFrameStage(*recovered, version.c_str()))
                recovered->removeCommitted(version.c_str());
            log("download slot=%s skipped=unchanged\n", SLOT_NAMES[slot]);
            strlcpy(result.frameIds[slot], info->id,
                    sizeof result.frameIds[slot]);
            result.frameOk[slot] = true;    // keep the current entry
            continue;
        }
        FsStore& store = recovered ? *recovered : partialStore();
        auto download = [&] {
            if (!recovered && millis() - passStarted >= TRANSFER_BUDGET_MS)
                return FrameDownload{FetchOutcome::NoProgress, "budget", 0, 0, 0, 0};
            if (!cache.staging || !cache.allocated[slot]) {
                const auto have = recovered ? int64_t(info->size) : store.size(target.c_str());
                return FrameDownload{FetchOutcome::NoProgress, "memory", 0,
                                     size_t(std::max(int64_t(0), have)), 0, 0};
            }
            if (recovered) return FrameDownload{FetchOutcome::Complete, "recovered", 0, 0, 0, 0};
            ScanScope activity(ScanActivity::Receive);
            auto fetched = receiveFrame(hostUrl, *info, SLOT_NAMES[slot], store, watchdog, passStarted,
                                        hostResult, &connection);
            if (fetched.outcome == FetchOutcome::Complete &&
                !store.moveCommitted(target.c_str(), version.c_str())) {
                fetched.outcome = FetchOutcome::NoProgress;
                fetched.reason = "storage";
            }
            return fetched;
        }();
        if (download.outcome == FetchOutcome::Complete) {
            const uint32_t decodeStart = millis();
            const auto decoded = [&] {
                ScanScope activity(ScanActivity::Inflate);
                auto file = store.openCommitted(version.c_str());
                if (!file || file.size() != info->size) return FrameDecode::Storage;
                return decodeFrameFile(info->encoding, file, &store, cache.staging, watchdog);
            }();
            log("inflate slot=%s bytes=%u ok=%d ms=%lu\n", SLOT_NAMES[slot],
                unsigned(info->size), decoded == FrameDecode::Complete,
                static_cast<unsigned long>(millis() - decodeStart));
            if (decoded == FrameDecode::Complete) {
                auto stage = frameStage(store, version, info->sha256);
                stage->unavailable = false;
                const FrameBody body{nullptr, info->size, stage};
                if (cache.publish(slot, info, body)) {
                    if (!queuedFrameStage(store, version.c_str())) frameStages.push_back(stage);
                } else {
                    download.outcome = FetchOutcome::NoProgress;
                    download.reason = "memory"; // the loop could not allocate a pinned-spare replacement
                }
            } else {
                download.outcome = decoded == FrameDecode::Invalid ? FetchOutcome::Failed : FetchOutcome::NoProgress;
                download.reason = decoded == FrameDecode::Memory ? "memory"
                    : decoded == FrameDecode::Storage ? "storage" : "invalid-frame";
                if (decoded == FrameDecode::Invalid && !queuedFrameStage(store, version.c_str()))
                    store.removeCommitted(version.c_str());
            }
            if (download.outcome == FetchOutcome::NoProgress && decoded != FrameDecode::Storage)
                download.have = info->size;
        }
        logFrameOutcome(SLOT_NAMES[slot], info->size, download);
        if (download.outcome == FetchOutcome::Complete) {
            strlcpy(result.frameIds[slot], info->id,
                    sizeof result.frameIds[slot]);
            result.frameOk[slot] = true;
        } else {
            result.framesPending = true;
            otherPending |= !download.reason || strcmp(download.reason, "memory") != 0;
            if (download.outcome == FetchOutcome::Failed) {
                result.frameFailed[slot] = true;
            }
        }
    }
    result.memoryOnlyPending = result.framesPending && !otherPending;
}

}  // namespace lcd
