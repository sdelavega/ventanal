// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega

// The production downloader and refresh pass, over the host fakes:
//  - every network read is bounded by the frame bytes still missing and a
//    response that keeps sending past the frame is rejected (never read
//    past the buffer, never accepted as a prefix);
//  - a stalled transfer gives up after its budget while the watchdog is
//    fed throughout (the worker stays supervised and progress is bounded);
//  - results stage for the main loop: new ids, dropped slots, skipped
//    frames, and the redraw decision after offline changes.

#include "../lcd43b/sync.h"
#include "../lcd43b/host_transport.h"
#include "../lcd43b/host_request.h"
#include "../lcd43b/inflate.h"
#include "../common/resume/storage.h"
#include "../lcd43b/installation.h"
#include "../common/resume/resume.h"
#include "../ventanal/manifest_signature.h"
// The host build always compiles sync.cpp against the public example
// configuration (VENTANAL_HOST_TEST), so the test asserts the same macro.
#include "../lcd43b/config.example.h"

#include "Arduino.h"
#include "esp_heap_caps.h"
#include "esp_sntp.h"
#include "HTTPClient.h"
#include "LittleFS.h"
#include "Preferences.h"
#include "WiFi.h"

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>
#include <ctime>
#include <zlib.h>

namespace {

struct CountingWatchdog : lcd::Watchdog {
    unsigned feeds = 0;
    void feed() override { ++feeds; }
};

lcd::FrameBody stagedBodies[lcd::SCREEN_SLOTS];
bool stagedDrops[lcd::SCREEN_SLOTS]{};
std::vector<uint8_t>* decodedCapture = nullptr;
int captureSlot = 0;
int rejectStageSlot = -1;

void refresh(const char* host, const char* device, const char ids[lcd::SCREEN_SLOTS][17],
             lcd::RefreshResult& result, CountingWatchdog& watchdog,
             const lcd::DeviceSettings& settings = {}, const char* skips = "",
             const char* ota = "", const char* approval = "",
             const char (*persisted)[17] = nullptr, int retryFileSaveSlot = -1) {
    std::vector<uint8_t> staging(lcd::FRAME_BYTES);
    lcd::ResidentCache cache;
    cache.staging = staging.data();
    if (retryFileSaveSlot >= 0) cache.retryFileSave[retryFileSaveSlot] = true;
    if (persisted) memcpy(cache.persistedIds, persisted, sizeof cache.persistedIds);
    std::fill_n(cache.allocated, lcd::SCREEN_SLOTS, true);
    static uint8_t* output;
    output = staging.data();
    cache.publish = [](int slot, const lcd::FrameInfo* info, const lcd::FrameBody& body) {
        if (info && slot == rejectStageSlot) return false;
        if (!info) stagedDrops[slot] = true;
        else {
            stagedBodies[slot] = body;
            if (decodedCapture && slot == captureSlot)
                decodedCapture->assign(output, output + lcd::FRAME_BYTES);
        }
        return true;
    };
    lcd::runRefresh(host, device, ids, result, watchdog, settings, skips, cache, ota, approval);
}

std::vector<uint8_t> compressFrame(const std::vector<uint8_t>& frame) {
    uLongf size = compressBound(frame.size());
    std::vector<uint8_t> compressed(size);
    assert(compress2(compressed.data(), &size, frame.data(), frame.size(), 9) == Z_OK);
    compressed.resize(size);
    return compressed;
}

std::vector<uint8_t> solidFrame(uint16_t rgb565) {
    std::vector<uint8_t> frame;
    frame.insert(frame.end(), {'V', 'N', 'R', '1'});
    frame.push_back(uint8_t(lcd::WIDTH & 0xFF));
    frame.push_back(uint8_t(lcd::WIDTH >> 8));
    frame.push_back(uint8_t(lcd::HEIGHT & 0xFF));
    frame.push_back(uint8_t(lcd::HEIGHT >> 8));
    frame.insert(frame.end(), 8, 0);
    frame.resize(lcd::FRAME_BYTES, 0);
    for (size_t i = 16; i < frame.size(); i += 2) {
        frame[i] = uint8_t(rgb565 >> 8);        // little-endian RGB565
        frame[i + 1] = uint8_t(rgb565 & 0xFF);
    }
    return frame;
}

std::string sha256Hex(const std::vector<uint8_t>& data) {
    Sha256 hash;
    hash.update(data.data(), data.size());
    const uint8_t* digest = hash.digest();
    char hex[65];
    for (int i = 0; i < 32; ++i)
        snprintf(hex + 2 * i, 3, "%02x", digest[i]);
    return std::string(hex, 64);
}

struct Entry {
    lcd::FrameInfo info;
    std::string url;
    std::string body;   // manifest fragment
};

Entry makeEntry(const std::string& name, const std::vector<uint8_t>& frame,
                lcd::FrameEncoding encoding = lcd::FrameEncoding::Raw) {
    Entry entry;
    const std::string sha = sha256Hex(frame);
    strlcpy(entry.info.id, sha.substr(0, 16).c_str(), sizeof entry.info.id);
    strlcpy(entry.info.sha256, sha.c_str(), sizeof entry.info.sha256);
    entry.url = "/devices/lcd43b/frames/" + name + "-"
              + std::string(entry.info.id) + ".vnr";
    strlcpy(entry.info.url, entry.url.c_str(), sizeof entry.info.url);
    entry.info.present = true;
    entry.info.encoding = encoding;
    entry.info.size = frame.size();
    const char* wireEncoding = encoding == lcd::FrameEncoding::Jpeg ? ",\"encoding\":\"jpeg\""
        : encoding == lcd::FrameEncoding::Zlib ? ",\"encoding\":\"zlib\"" : "";
    entry.body = "{\"id\":\"" + std::string(entry.info.id) + "\",\"url\":\""
               + entry.url + "\",\"size\":" + std::to_string(frame.size())
               + wireEncoding
               + ",\"sha256\":\"" + sha + "\"}";
    return entry;
}

bool receiveFrame(const Entry& entry, std::vector<uint8_t>& buffer, CountingWatchdog& watchdog,
                  const char* reason = nullptr) {
    auto& store = partialStore();
    const auto download = lcd::receiveFrame("http://host", entry.info, "art", store, watchdog, millis());
    if (reason) assert(download.reason && !strcmp(download.reason, reason));
    return download.outcome == FetchOutcome::Complete &&
        store.takeCommitted("frame-art", buffer.data(), entry.info.size);
}

std::string manifestBody(const Entry& art, const Entry& weather) {
    return "{\"version\":1,\"screens\":{\"art\":" + art.body
         + ",\"weather\":" + weather.body
         + "},\"quiet\":{\"start\":\"23:00\",\"end\":\"06:00\"}}";
}

void resetFakes() {
    WiFi = FakeWiFi{};
    requestedUrls.clear();
    files.clear();
    preferences.clear();
    Serial.output.clear();
    response.clear();
    urlResponses.clear();
    responseBody.clear();
    status = 200;
    failBegin = false;
    stallBody = false;
    sizeFromResponse = false;
    advertisedSize = 0;
    largestRead = 0;
    frameReadCalls = 0;
    frameBytesRead = 0;
    fakeGetResponse = nullptr;
    lastHeaders.clear();
    fakeClock = 0;
    fakeSyncStatus = SNTP_SYNC_STATUS_COMPLETED;
    delayedFrom = 0;
    delayedUntilMs = 0;
    holdConnection = false;
    reuse = true;
    lastConnectionHeader.clear();
    persistentServer = false;
    failAllocation = false;
    psramLargest = psramCapacity = SIZE_MAX;
    psramPeak = 0;
    assert(!psramInUse);
    failRename = false;
    std::fill_n(stagedBodies, lcd::SCREEN_SLOTS, lcd::FrameBody{});
    std::fill_n(stagedDrops, lcd::SCREEN_SLOTS, false);
    rejectStageSlot = -1;
    fakeTz.clear();
    socketTestMode = socketOpen = false;
    socketConnects = socketCloses = 0; fakeSocketTable.clear(); fakeSocket = -1;
    fakeHeaderBytes = 0;
    fakeFreeInternal = fakeLargestInternal = 0;
}

bool cutFirstPass = false;
std::vector<uint32_t> rangeStarts;
uint64_t lastCounter = 0;

void serveRanges(const Entry& art, const Entry& weather,
                 const std::vector<uint8_t>& artBytes, const char* origin = "http://host") {
    responseBody = manifestBody(art, weather);
    urlResponses[origin + art.url] = artBytes;
    sizeFromResponse = true;
    rangeStarts.clear();
    lastCounter = 0;
    fakeGetResponse = [] {
        if (lastUrl.compare(0, 8, "https://") == 0) {
            const auto path = lastUrl.substr(lastUrl.find('/', 8));
            const auto key = decodeDeviceKey(lcd::installation.deviceKey);
            const auto counter = std::stoull(lastHeaders.at("X-Ventanal-Counter"));
            assert(counter > lastCounter);
            assert(lastHeaders.at("Authorization") == "Ventanal " +
                   requestProof(key, "GET", path.c_str(), counter));
            assert(lastHeaders.at("X-Ventanal-Device") == "test-device");
            lastCounter = counter;
        }
        if (lastUrl.find("/frames/") == std::string::npos) return 200;
        const auto range = lastHeaders.find("Range");
        if (range == lastHeaders.end()) return 200;
        unsigned start = 0, end = 0;
        assert(sscanf(range->second.c_str(), "bytes=%u-%u", &start, &end) == 2);
        const auto etag = "\"" + lastUrl.substr(lastUrl.size() - 20, 16) + "\"";
        if (start) assert(lastHeaders.at("If-Range") == etag);
        rangeStarts.push_back(start);
        if (cutFirstPass && start == kResumeChunk) {
            fakeClock += lcd::TRANSFER_BUDGET_MS;
            return HTTPC_ERROR_READ_TIMEOUT;
        }
        assert(start <= end && end < response.size() && end - start < kResumeChunk);
        const auto total = response.size();
        fakeResponseHeaders["ETag"] = etag;
        fakeResponseHeaders["Content-Range"] = "Bytes " + std::to_string(start) + "-"
            + std::to_string(end) + "/" + std::to_string(total);
        const auto first = response.begin() + start;
        response = std::vector<uint8_t>(first, response.begin() + end + 1);
        return 206;
    };
}

void host_frame_unfinished_is_not_failed(const lcd::RefreshResult& result,
                                         const std::vector<uint8_t>& previous) {
    assert(!result.frameOk[0] && !result.frameFailed[0] && !result.frameIds[0][0]);
    assert(!stagedBodies[0].available() && files.at("/art.vnr") == previous);
    assert(!lcd::anyRequiredFrameFailed(result));
    const auto expected = "frame slot=art outcome=progress reason=budget got="
        + std::to_string(kResumeChunk) + " have=" + std::to_string(kResumeChunk);
    assert(Serial.output.find(expected + " total=") != std::string::npos);
}

void host_frame_resumes_across_passes() {
    resetFakes();
    const auto frame = solidFrame(0xF800);
    const auto previous = solidFrame(0x07E0);
    const Entry art = makeEntry("art", frame);
    const Entry weather = makeEntry("weather", previous);
    const Entry spare = makeEntry("spare", previous);
    serveRanges(art, weather, frame);
    responseBody.insert(responseBody.size() - 1, ",\"spares\":[" + spare.body + "]");
    files["/art.vnr"] = previous;
    char ids[lcd::SCREEN_SLOTS][17]{};
    strcpy(ids[0], "aaaaaaaaaaaaaaaa");
    strcpy(ids[1], weather.info.id);
    CountingWatchdog watchdog;
    lcd::RefreshResult result;
    cutFirstPass = true;
    refresh("http://host", "lcd43b", ids, result, watchdog);
    assert(frameBytesRead == kResumeChunk);
    assert(files.at("/partials/frame-art.part").size() == kResumeChunk);
    host_frame_unfinished_is_not_failed(result, previous);
    assert(std::count(requestedUrls.begin(), requestedUrls.end(), "http://host" + spare.url) == 0);
    assert(!result.frameOk[3] && !result.frameFailed[3]);
    assert(Serial.output.find("frame slot=spare0 outcome=no-progress reason=budget got=0 have=0")
           != std::string::npos);

    cutFirstPass = false;
    psramLargest = 344052;
    rangeStarts.clear();
    Serial.output.clear();
    strcpy(ids[3], spare.info.id);
    refresh("http://host", "lcd43b", ids, result, watchdog);
    assert(!rangeStarts.empty() && rangeStarts.front() == kResumeChunk);
    assert(frameBytesRead == frame.size());
    assert(result.frameOk[0] && !result.frameFailed[0] && !strcmp(result.frameIds[0], art.info.id));
    assert(stagedBodies[0].size == frame.size());
    assert(!psramInUse && psramPeak == kResumeChunk);
    printf("streamed raw (downloaded): peak PSRAM %zu B\n", psramPeak);
    assert(files.at("/partials/" + stagedBodies[0].file->target) == frame);
    assert(files.size() == 2 && files.at("/art.vnr") == previous);
    const auto total = std::to_string(frame.size());
    assert(Serial.output.find("resume target=frame-art from=" + std::to_string(kResumeChunk)
                              + " total=" + total + "\n") != std::string::npos);
    const auto done = "resume done target=frame-art bytes=" + total + " ms=";
    const auto doneAt = Serial.output.find(done);
    assert(doneAt != std::string::npos);
    const auto firstNonDigit = Serial.output.find_first_not_of("0123456789", doneAt + done.size());
    assert(firstNonDigit != std::string::npos && firstNonDigit > doneAt + done.size());
    assert(Serial.output[firstNonDigit] == '\n');
    const auto expected = "frame slot=art outcome=complete reason=ok got="
        + std::to_string(frame.size() - kResumeChunk) + " have=0 total=" + total + " ms=";
    const auto outcome = Serial.output.find(expected);
    assert(outcome != std::string::npos);
    assert(Serial.output.find("frame slot=art outcome=", outcome + 1) == std::string::npos);
}

void host_frame_hash_mismatch_fails() {
    resetFakes();
    const auto frame = solidFrame(0xF800);
    const auto previous = solidFrame(0x07E0);
    const Entry art = makeEntry("art", frame);
    const Entry weather = makeEntry("weather", previous);
    auto corrupt = frame;
    corrupt.back() ^= 1;
    const auto originalInstallation = lcd::installation;
    strcpy(lcd::installation.deviceKey, std::string(64, 'a').c_str());
    strcpy(lcd::installation.pairing, "test-device");
    strcpy(lcd::installation.certificate, "fake certificate for the fake HTTP client");
    serveRanges(art, weather, corrupt, "https://host");
    JsonDocument manifest;
    assert(!deserializeJson(manifest, responseBody));
    const auto key = decodeDeviceKey(lcd::installation.deviceKey);
    manifest["sig"] = hmacHex(key, canonicalJson(manifest.as<JsonVariantConst>()));
    responseBody.clear();
    serializeJson(manifest, responseBody);
    files["/art.vnr"] = previous;
    char ids[lcd::SCREEN_SLOTS][17]{};
    strcpy(ids[0], "aaaaaaaaaaaaaaaa");
    strcpy(ids[1], weather.info.id);
    CountingWatchdog watchdog;
    lcd::RefreshResult result;
    refresh("https://host", "lcd43b", ids, result, watchdog);
    lcd::installation = originalInstallation;
    assert(result.manifestOk && lastCounter > 1);
    assert(frameBytesRead == frame.size());
    assert(!result.frameOk[0] && result.frameFailed[0] && !stagedBodies[0].available());
    assert(files.size() == 1 && files.at("/art.vnr") == previous);
    const auto total = std::to_string(frame.size());
    assert(Serial.output.find("frame slot=art outcome=failed reason=hash got=" + total
                              + " have=0 total=" + total + " ms=") != std::string::npos);
}

void host_range_transport_parses_416() {
    resetFakes();
    fakeGetResponse = [] {
        assert(lastHeaders.at("Range") == "bytes=1234-1234");
        fakeResponseHeaders["Content-Range"] = "bytes */1234";
        response = {1, 2, 3};  // an error body must not reach the sink
        return 416;
    };
    const std::string key;
    CountingWatchdog watchdog;
    lcd::HostRangeTransport transport("http://host", "/frames/art.vnr", key, watchdog, millis());
    const ResumeSource source{"http://host/frames/art.vnr", "frame-id", "", 1234, true};
    bool sawHeaders = false;
    assert(transport.get(source, 1234, 1234, "frame-id", [&](const RangeResponse& headers) {
        assert(!sawHeaders && headers.status == 416 && headers.total == 1234);
        sawHeaders = true;
        return true;
    }, [](const uint8_t*, size_t) {
        assert(false && "unexpected 416 body");
        return false;
    }));
    assert(sawHeaders && !transport.malformed() && transport.bytesRead() == 0);
}

bool receiveFramed206(bool shortBody) {
    resetFakes();
    holdConnection = !shortBody;
    fakeGetResponse = [] {
        fakeResponseHeaders["Content-Range"] = "bytes 0-2/6";
        return 206;
    };
    advertisedSize = 3;
    response = shortBody ? std::vector<uint8_t>{1, 2} : std::vector<uint8_t>{1, 2, 3};
    const std::string key;
    CountingWatchdog watchdog;
    lcd::HostRangeTransport transport("http://host", "/frames/art.vnr", key, watchdog, millis());
    const ResumeSource source{"http://host/frames/art.vnr", "frame-id", "", 6, true};
    size_t received = 0;
    const uint32_t started = millis();
    const bool complete = transport.get(source, 0, 2, "", [](const RangeResponse& headers) {
        assert(headers.status == 206 && headers.total == 6);
        return true;
    }, [&](const uint8_t*, size_t count) {
        received += count;
        return true;
    });
    assert(received == response.size());
    assert(millis() - started < 50);
    return complete;
}

void framed_206_accepted_without_close() { assert(receiveFramed206(false)); }
void short_206_still_rejected() { assert(!receiveFramed206(true)); }

}  // namespace

void recover_committed_frames_against_manifest() {
    resetFakes();
    const auto artFrame = solidFrame(0xf800);
    const auto weatherFrame = solidFrame(0x07e0);
    const auto art = makeEntry("art", artFrame);
    const auto weather = makeEntry("weather", weatherFrame);
    responseBody = manifestBody(art, weather);
    urlResponses["http://host/devices/lcd43b/manifest.json?board=lcd43b&fw=1.0.0&test_release_key=1"] =
        {responseBody.begin(), responseBody.end()};
    sizeFromResponse = true;
    assert(LittleFS.exists("/partials") || LittleFS.mkdir("/partials"));
    files["/partials/frame-art"] = artFrame;
    files["/partials/frame-weather"] = artFrame; // right size, wrong hash
    urlResponses["http://host" + weather.url] = weatherFrame;
    char ids[lcd::SCREEN_SLOTS][17]{};
    lcd::RefreshResult result;
    CountingWatchdog watchdog;
    refresh("http://host", "lcd43b", ids, result, watchdog);
    assert(result.manifestOk && result.frameOk[0] && result.frameOk[1]);
    assert(std::find(requestedUrls.begin(), requestedUrls.end(), "http://host" + art.url) == requestedUrls.end());
    const auto artStage = "/partials/" + stagedBodies[0].file->target;
    assert(files.count(artStage) && !files.count("/partials/frame-weather"));
    stagedBodies[0] = {}; // the save completed before the next manifest
    memcpy(ids, result.frameIds, sizeof ids);
    refresh("http://host", "lcd43b", ids, result, watchdog, {}, "", "", "", ids);
    assert(result.frameOk[0] && !files.count(artStage));
}

static std::vector<uint8_t> jpegOfSize(size_t size) {
    std::ifstream input("fixtures/lcd43b-host-art.jpg", std::ios::binary);
    std::vector<uint8_t> jpeg{std::istreambuf_iterator<char>(input), {}};
    assert(input.is_open() && jpeg.size() <= size);
    // Insert legal comment segments before the fixture's original markers.
    std::vector<uint8_t> padded{0xff, 0xd8};
    for (size_t remaining = size - jpeg.size(); remaining;) {
        const size_t segment = std::min<size_t>(remaining, 32000);
        assert(segment >= 4);
        padded.insert(padded.end(), {0xff, 0xfe, uint8_t((segment - 2) >> 8), uint8_t(segment - 2)});
        padded.resize(padded.size() + segment - 4, 0x55);
        remaining -= segment;
    }
    padded.insert(padded.end(), jpeg.begin() + 2, jpeg.end());
    return padded;
}

static void saveAndCheck(int slot, const Entry& entry, const std::vector<uint8_t>& wire) {
    Preferences prefs;
    assert(prefs.begin("ventanal", false));
    lcd::CacheLocation location;
    lcd::DeferredSave save;
    assert(save.replace(entry.info.id, stagedBodies[slot], 0));
    assert(lcd::persistCacheSlot(prefs, slot, location, save));
    assert(!strcmp(location.id, entry.info.id));
    assert(files.at(lcd::residentPath(slot, location.alternate).c_str()) == wire);
    stagedBodies[slot] = {};
}

void spare_larger_than_free_psram_downloads() {
    resetFakes();
    const auto wire = jpegOfSize(514703);
    const auto spare = makeEntry("spare", wire, lcd::FrameEncoding::Jpeg);
    const auto art = makeEntry("art", solidFrame(0xf800));
    const auto weather = makeEntry("weather", solidFrame(0x07e0));
    responseBody = manifestBody(art, weather);
    responseBody.insert(responseBody.size() - 1, ",\"spares\":[" + spare.body + "]");
    urlResponses["http://host" + spare.url] = wire;
    sizeFromResponse = true;
    psramLargest = 344052;
    psramCapacity = 380664;
    char ids[lcd::SCREEN_SLOTS][17]{};
    strcpy(ids[0], art.info.id);
    strcpy(ids[1], weather.info.id);
    lcd::RefreshResult result;
    CountingWatchdog watchdog;
    refresh("http://host", "lcd43b", ids, result, watchdog);
    assert(result.frameOk[3] && !result.frameFailed[3]);
    assert(!files.count("/spare0-alt.vnr"));
    saveAndCheck(3, spare, wire);
    assert(!psramInUse && psramPeak == kResumeChunk);
    printf("spare_larger_than_free_psram_downloads: peak PSRAM %zu B\n", psramPeak);
}

void two_large_frames_in_one_pass() {
    resetFakes();
    const auto artWire = jpegOfSize(309257);
    const auto spareWire = jpegOfSize(320246);
    const auto art = makeEntry("art", artWire, lcd::FrameEncoding::Jpeg);
    const auto spare = makeEntry("spare", spareWire, lcd::FrameEncoding::Jpeg);
    const auto weather = makeEntry("weather", solidFrame(0x07e0));
    serveRanges(art, weather, artWire);
    responseBody.insert(responseBody.size() - 1, ",\"spares\":[" + spare.body + "]");
    urlResponses["http://host" + spare.url] = spareWire;
    psramLargest = 344052;
    psramCapacity = 380664;
    char ids[lcd::SCREEN_SLOTS][17]{};
    strcpy(ids[1], weather.info.id);
    lcd::RefreshResult result;
    CountingWatchdog watchdog;
    refresh("http://host", "lcd43b", ids, result, watchdog);
    assert(result.frameOk[0] && result.frameOk[3] && !lcd::anyRequiredFrameFailed(result));
    assert(!files.count("/art-alt.vnr") && !files.count("/spare0-alt.vnr"));
    saveAndCheck(0, art, artWire);
    saveAndCheck(3, spare, spareWire);
    assert(!psramInUse && psramPeak == kResumeChunk);
    printf("two_large_frames_in_one_pass: peak PSRAM %zu B, deferred %zu B\n", psramPeak, psramInUse);
}

void memory_shortage_is_no_progress() {
    resetFakes();
    const auto wire = jpegOfSize(514703);
    const auto art = makeEntry("art", wire, lcd::FrameEncoding::Jpeg);
    const auto weather = makeEntry("weather", solidFrame(0x07e0));
    serveRanges(art, weather, wire);
    char ids[lcd::SCREEN_SLOTS][17]{};
    strcpy(ids[1], weather.info.id);
    lcd::RefreshResult result;
    CountingWatchdog watchdog;
    cutFirstPass = true;
    refresh("http://host", "lcd43b", ids, result, watchdog);
    cutFirstPass = false;
    const auto checkpoint = files.at("/partials/frame-art.meta");
    const auto partial = files.at("/partials/frame-art.part");
    failAllocation = true;
    Serial.output.clear();
    refresh("http://host", "lcd43b", ids, result, watchdog);
    assert(result.framesPending && result.memoryOnlyPending && !lcd::anyRequiredFrameFailed(result));
    assert(Serial.output.find("frame slot=art outcome=no-progress reason=memory") != std::string::npos);
    assert(files.at("/partials/frame-art.meta") == checkpoint);
    assert(files.at("/partials/frame-art.part") == partial);

    // Once verified, even failure to allocate the decoder pool keeps the
    // completed file, which the next pass can decode without a network GET.
    partialStore().discard("frame-art");
    files["/partials/frame-art"] = wire;
    frameBytesRead = 0;
    Serial.output.clear();
    refresh("http://host", "lcd43b", ids, result, watchdog);
    const std::string version = std::string("/partials/frame-art.") + art.info.id;
    assert(result.framesPending && result.memoryOnlyPending && !result.frameFailed[0]);
    assert(Serial.output.find("frame slot=art outcome=no-progress reason=memory") != std::string::npos);
    assert(!frameBytesRead && files.at(version) == wire);
    failAllocation = false;
    refresh("http://host", "lcd43b", ids, result, watchdog);
    assert(result.frameOk[0] && !frameBytesRead);
    puts("memory_shortage_is_no_progress: download checkpoint and committed JPEG retained");
}

static void decodedPixelsMatch(const std::vector<uint8_t>& wire, lcd::FrameEncoding encoding,
                               bool recovered) {
    resetFakes();
    const auto art = makeEntry("art", wire, encoding);
    const auto weather = makeEntry("weather", solidFrame(0x07e0));
    serveRanges(art, weather, wire);
    if (recovered) {
        files["/partials/frame-art"] = wire;
        urlResponses.clear();
    }
    char ids[lcd::SCREEN_SLOTS][17]{};
    strcpy(ids[1], weather.info.id);
    std::vector<uint8_t> reference(lcd::FRAME_BYTES);
    CountingWatchdog watchdog;
    if (encoding == lcd::FrameEncoding::Jpeg)
        assert(lcd::decodeHostJpeg(wire.data(), wire.size(), reference.data(), watchdog));
    else
        assert(lcd::decodeFrame(encoding, wire.data(), wire.size(), reference.data()));
    std::vector<uint8_t> decoded;
    decodedCapture = &decoded;
    psramCapacity = 380664;
    lcd::RefreshResult result;
    refresh("http://host", "lcd43b", ids, result, watchdog);
    decodedCapture = nullptr;
    assert(result.frameOk[0] && decoded == reference);
    assert((frameBytesRead == 0) == recovered);
    assert(!psramInUse);
    const size_t expected = encoding == lcd::FrameEncoding::Zlib ? kResumeChunk + sizeof(tinfl_decompressor)
        : recovered ? 0 : kResumeChunk;
    assert(psramPeak == expected);
    printf("streamed %s (%s): peak PSRAM %zu B\n",
           encoding == lcd::FrameEncoding::Jpeg ? "jpeg" : "zlib",
           recovered ? "recovered" : "downloaded", psramPeak);
}

void streamed_jpeg_decode_matches_buffer_decode() {
    const auto jpeg = jpegOfSize(514703);
    for (bool recovered : {false, true}) decodedPixelsMatch(jpeg, lcd::FrameEncoding::Jpeg, recovered);
}

void streamed_zlib_decode_matches_buffer_decode() {
    auto frame = solidFrame(0x07e0);
    uint32_t seed = 1;
    for (size_t i = lcd::HEADER_BYTES; i < 210000; ++i) {
        seed = seed * 1664525 + 1013904223;
        frame[i] = uint8_t(seed >> 24);
    }
    const auto zipped = compressFrame(frame);
    assert(zipped.size() > kResumeChunk);
    for (bool recovered : {false, true}) decodedPixelsMatch(zipped, lcd::FrameEncoding::Zlib, recovered);
}

void deferred_file_survives_replacement_and_cache_failure() {
    resetFakes();
    const auto oldWire = jpegOfSize(309257);
    const auto newWire = jpegOfSize(320246);
    const auto oldArt = makeEntry("art", oldWire, lcd::FrameEncoding::Jpeg);
    const auto newArt = makeEntry("art", newWire, lcd::FrameEncoding::Jpeg);
    const auto weather = makeEntry("weather", solidFrame(0x07e0));
    char ids[lcd::SCREEN_SLOTS][17]{};
    strcpy(ids[1], weather.info.id);
    CountingWatchdog watchdog;
    lcd::RefreshResult result;
    serveRanges(oldArt, weather, oldWire);
    refresh("http://host", "lcd43b", ids, result, watchdog);
    assert(result.frameOk[0]);
    auto pinned = stagedBodies[0];
    const auto oldStage = "/partials/" + pinned.file->target;
    strcpy(ids[0], oldArt.info.id);
    serveRanges(newArt, weather, newWire);
    refresh("http://host", "lcd43b", ids, result, watchdog);
    assert(result.frameOk[0] && files.at(oldStage) == oldWire);
    const auto newStage = "/partials/" + stagedBodies[0].file->target;

    Preferences prefs;
    assert(prefs.begin("ventanal", false));
    lcd::CacheLocation current{ "aaaaaaaaaaaaaaaa", false };
    assert(lcd::saveCacheLocation(prefs, 0, current));
    files["/art.vnr"] = {1};
    lcd::DeferredSave save;
    assert(save.replace(newArt.info.id, stagedBodies[0], 0));

    Preferences::failStringWrite = true;
    assert(!lcd::persistCacheSlot(prefs, 0, current, save));
    assert(!current.alternate && files.at("/art.vnr")[0] == 1 && files.at(newStage) == newWire);
    Preferences::failStringWrite = false;
    assert(lcd::persistCacheSlot(prefs, 0, current, save));
    assert(current.alternate && !files.count("/art.vnr") && files.at("/art-alt.vnr") == newWire);
    assert(lcd::writeCachedFrame("/override.vnr", pinned));
    assert(files.at("/override.vnr") == oldWire);

    save = {};
    stagedBodies[0] = {};
    strcpy(ids[0], newArt.info.id);
    refresh("http://host", "lcd43b", ids, result, watchdog);
    assert(!files.count(newStage) && files.count(oldStage));
    pinned = {};
    refresh("http://host", "lcd43b", ids, result, watchdog);
    assert(!files.count(oldStage));
}

void lost_sd_source_retries_unchanged_frame_on_flash() {
    resetFakes();
    static fs::FS sd;
    setPartialSdFilesystem(sd);
    selectPartialStore(true);
    const auto wire = jpegOfSize(309257);
    const auto art = makeEntry("art", wire, lcd::FrameEncoding::Jpeg);
    const auto weather = makeEntry("weather", solidFrame(0x07e0));
    serveRanges(art, weather, wire);
    char ids[lcd::SCREEN_SLOTS][17]{};
    strcpy(ids[1], weather.info.id);
    CountingWatchdog watchdog;
    lcd::RefreshResult result;
    refresh("http://host", "lcd43b", ids, result, watchdog);
    assert(result.frameOk[0] && !partialsOnFlash());
    const auto lost = stagedBodies[0];
    assert(sd.files.at("/partials/" + lost.file->target) == wire);

    sd.offline = true;
    assert(!lcd::writeCachedFrame("/art.vnr", lost));
    assert(lost.file->unavailable && partialsOnFlash() && !files.count("/art.vnr"));
    strcpy(ids[0], art.info.id);
    refresh("http://host", "lcd43b", ids, result, watchdog, {}, "", "", "", nullptr, 0);
    assert(result.frameOk[0] && !result.frameFailed[0]);
    assert(&stagedBodies[0].file->store == &flashPartialStore());
    saveAndCheck(0, art, wire);
    sd.offline = false;
}

void join_uses_strongest_scanned_ap() {
    resetFakes(); WiFi.connected = false;
    WiFi.aps = {{-80, 1, {1,2,3,4,5,6}}, {-42, 11, {6,5,4,3,2,1}}};
    CountingWatchdog watchdog;
    assert(lcd::connectForUpdates(watchdog));
    assert(WiFi.pinnedBegins == 1 && WiFi.plainBegins == 0);
    assert(WiFi.selectedChannel == 11 && WiFi.selectedBssid == WiFi.aps[1].bssid);
}

void join_falls_back_when_bssid_join_fails() {
    resetFakes(); WiFi.connected = false; WiFi.failPinned = true;
    WiFi.aps = {{-45, 6, {1,2,3,4,5,6}}};
    CountingWatchdog watchdog;
    assert(lcd::connectForUpdates(watchdog));
    assert(WiFi.pinnedBegins == 1 && WiFi.plainBegins == 1);
}

void scan_log_has_no_identifiers() {
    resetFakes(); WiFi.connected = false;
    WiFi.aps = {{-45, 6, {1,2,3,4,5,6}}};
    CountingWatchdog watchdog;
    assert(lcd::connectForUpdates(watchdog));
    assert(Serial.output.find("wifi scan aps=1 best_dbm=-45 best_ch=6 joined_dbm=-45 joined_ch=6") != std::string::npos);
    assert(Serial.output.find("01:02:03:04:05:06") == std::string::npos);
}

int hostFailureResponse() { return requestedUrls.size() == 1 ? 200 : -1; }
void host_failures_trigger_one_rejoin_per_pass() {
    resetFakes();
    WiFi.aps = {{-45, 6, {1,2,3,4,5,6}}};
    const auto frame = solidFrame(0xf800);
    const auto art = makeEntry("art", frame), weather = makeEntry("weather", frame);
    responseBody = manifestBody(art, weather); sizeFromResponse = true;
    responseBody.insert(responseBody.size() - 1, ",\"spares\":[" + art.body + "]");
    fakeGetResponse = hostFailureResponse;
    char ids[lcd::SCREEN_SLOTS][17]{}; lcd::RefreshResult result; CountingWatchdog watchdog;
    refresh("http://host", "lcd43b", ids, result, watchdog);
    assert(result.manifestOk && WiFi.disconnects == 1 && WiFi.pinnedBegins == 1);
    assert(requestedUrls.size() == 4);
    assert(Serial.output.find("wifi rejoin reason=host-unreachable") != std::string::npos);
    requestedUrls.clear();
    refresh("http://host", "lcd43b", ids, result, watchdog);
    assert(result.manifestOk && WiFi.disconnects == 2 && WiFi.pinnedBegins == 2);
}

void rejoin_keeps_association_when_scan_empty() {
    resetFakes();
    WiFi.scanCounts = {0, 0, 0};
    const auto frame = solidFrame(0xf800);
    const auto art = makeEntry("art", frame), weather = makeEntry("weather", frame);
    responseBody = manifestBody(art, weather); sizeFromResponse = true;
    responseBody.insert(responseBody.size() - 1, ",\"spares\":[" + art.body + "]");
    fakeGetResponse = hostFailureResponse;
    char ids[lcd::SCREEN_SLOTS][17]{}; lcd::RefreshResult result; CountingWatchdog watchdog;
    refresh("http://host", "lcd43b", ids, result, watchdog);
    assert(result.manifestOk && WiFi.connected && WiFi.disconnects == 0);
    assert(WiFi.scans >= 3 && WiFi.pinnedBegins == 0 && WiFi.plainBegins == 0);
    assert(requestedUrls.size() == 4);
}

void rejoin_retries_empty_scan() {
    resetFakes();
    WiFi.aps = {{-45, 6, {1,2,3,4,5,6}}};
    WiFi.scanCounts = {0, 0, 0, 1};
    const auto frame = solidFrame(0xf800);
    const auto art = makeEntry("art", frame), weather = makeEntry("weather", frame);
    responseBody = manifestBody(art, weather); sizeFromResponse = true;
    responseBody.insert(responseBody.size() - 1, ",\"spares\":[" + art.body + "]");
    fakeGetResponse = hostFailureResponse;
    char ids[lcd::SCREEN_SLOTS][17]{}; lcd::RefreshResult result; CountingWatchdog watchdog;
    refresh("http://host", "lcd43b", ids, result, watchdog);
    assert(result.manifestOk && WiFi.connected && WiFi.scans == 4);
    assert(WiFi.disconnects == 1 && WiFi.pinnedBegins == 1);
}

void rejoin_failure_restores_wifi() {
    resetFakes();
    WiFi.aps = {{-45, 6, {1,2,3,4,5,6}}};
    WiFi.failPinned = true; WiFi.failedPlainBegins = 1;
    const auto frame = solidFrame(0xf800);
    const auto art = makeEntry("art", frame), weather = makeEntry("weather", frame);
    responseBody = manifestBody(art, weather); sizeFromResponse = true;
    responseBody.insert(responseBody.size() - 1, ",\"spares\":[" + art.body + "]");
    fakeGetResponse = hostFailureResponse;
    char ids[lcd::SCREEN_SLOTS][17]{}; lcd::RefreshResult result; CountingWatchdog watchdog;
    refresh("http://host", "lcd43b", ids, result, watchdog);
    assert(result.manifestOk && WiFi.connected);
    assert(WiFi.pinnedBegins == 1 && WiFi.plainBegins == 2);
    assert(requestedUrls.size() == 4);
}

// A transport-level manifest failure while Wi-Fi reports connected ends
// the pass's only host connection, so it re-joins (once) and fetches the
// manifest once more; the second failure does not re-join again.
void post_wake_failure_does_full_wifi_restart() {
    resetFakes(); lcd::networkRecovery = {};
    lcd::networkRecovery.woke();
    WiFi.aps = {{-45, 6, {1,2,3,4,5,6}}};
    status = HTTPC_ERROR_CONNECTION_REFUSED;
    char ids[lcd::SCREEN_SLOTS][17]{}; lcd::RefreshResult result; CountingWatchdog watchdog;
    refresh("http://host", "lcd43b", ids, result, watchdog);
    assert(WiFi.disconnects == 1 && WiFi.lastDisconnectOff && WiFi.offModes == 1 && WiFi.staModes == 1);
    assert(requestedUrls.size() == 2);
    assert(Serial.output.find("wifi restart reason=post-wake-unreachable ok=1") != std::string::npos);
}
void non_wake_failure_keeps_plain_rejoin() {
    resetFakes(); lcd::networkRecovery = {};
    WiFi.aps = {{-45, 6, {1,2,3,4,5,6}}}; status = HTTPC_ERROR_CONNECTION_REFUSED;
    char ids[lcd::SCREEN_SLOTS][17]{}; lcd::RefreshResult result; CountingWatchdog watchdog;
    refresh("http://host", "lcd43b", ids, result, watchdog);
    assert(WiFi.disconnects == 1 && !WiFi.lastDisconnectOff && WiFi.offModes == 0);
}
void success_clears_wake_flag() {
    lcd::NetworkRecovery state; state.woke(); state.response(200);
    assert(!state.postWake && !state.rebootDue());
}
void reboot_after_one_failed_pass_post_restart() {
    resetFakes(); lcd::networkRecovery = {}; lcd::networkRecovery.woke();
    WiFi.aps = {{-45, 6, {1,2,3,4,5,6}}}; status = HTTPC_ERROR_CONNECTION_REFUSED;
    char ids[lcd::SCREEN_SLOTS][17]{}; lcd::RefreshResult result; CountingWatchdog watchdog;
    refresh("http://host", "lcd43b", ids, result, watchdog);
    assert(!lcd::networkRecovery.rebootDue());
    refresh("http://host", "lcd43b", ids, result, watchdog);
    assert(lcd::networkRecovery.rebootDue());
    lcd::networkRecovery = {};
}
void post_wake_failure_retries_within_60s() {
    lcd::NetworkRecovery state; state.woke(); state.failure();
    assert(state.retryInterval(300000) == 60000);
    state.response(200); assert(state.retryInterval(300000) == 300000);
}
void reboot_allowed_after_proven_previous_reboot() {
    Preferences p; p.begin("ventanal", false); preferences.erase("net-pending");
    lcd::RecoveryRebootGate gate; gate.load(p, false);
    assert(gate.allowed(true, false, false, false));
    assert(gate.markReboot(p));
    assert(gate.recordSuccess(p));
    lcd::RecoveryRebootGate next; next.load(p, false);
    assert(next.allowed(true, false, false, false));
}
void reboot_blocked_when_previous_unproven() {
    Preferences p; p.begin("ventanal", false); preferences.erase("net-pending");
    lcd::RecoveryRebootGate gate; gate.load(p, false); assert(gate.markReboot(p));
    lcd::RecoveryRebootGate next; next.load(p, false);
    assert(!next.allowed(true, false, false, false));
    lcd::RecoveryRebootGate rtc; preferences.erase("net-pending"); rtc.load(p, true);
    assert(!rtc.allowed(true, false, false, false));
}
void success_clears_pending_flag() {
    Preferences p; p.begin("ventanal", false); preferences.erase("net-pending");
    lcd::RecoveryRebootGate gate; gate.load(p, false); assert(gate.markReboot(p));
    assert(gate.recordSuccess(p));
    assert(!gate.pending && gate.allowed(true, false, false, false));
    lcd::NetworkRecovery state; state.response(206); assert(state.successfulResponse);
}
void reboot_gate_failed_writes_stay_blocked() {
    Preferences p; p.begin("ventanal", false); preferences.erase("net-pending");
    lcd::RecoveryRebootGate gate; gate.load(p, false);
    Preferences::failWriteKey = "net-pending";
    assert(!gate.markReboot(p)); assert(gate.pending);
    assert(!gate.recordSuccess(p)); assert(gate.pending);
    Preferences::failWriteKey.clear();
    assert(gate.recordSuccess(p)); assert(!gate.pending);
}
void socket_diagnostics_are_distinct() {
    lcd::DiagStorage storage{}; lcd::DiagRing ring(storage);
    ring.socketCreated(3, 54, 0); ring.socketConnected(4, 54, -1, 9);
    Serial.output.clear(); ring.printUnread();
    assert(Serial.output.find("socket fd=54 socket_errno=0") != std::string::npos);
    assert(Serial.output.find("socket_connect fd=54 result=-1 connect_errno=9") != std::string::npos);
    ring.socketSelected(5, 1, 0, 110, 5000, true); ring.socketError(6, 54, 0, 0, 113);
    Serial.output.clear(); ring.printUnread();
    assert(Serial.output.find("socket_select result=1 select_errno=0 elapsed_ms=110 timeout_ms=5000 writable=1") != std::string::npos);
    assert(Serial.output.find("socket_error fd=54 result=0 getsockopt_errno=0 so_error=113") != std::string::npos);
    ring.tlsSetupFailed(7, -0x7F00, 70000, 31000);
    Serial.output.clear(); ring.printUnread();
    assert(Serial.output.find("tls_setup code=-32512 (-0x7F00) free_internal=70000 largest_internal=31000") != std::string::npos);
}
void reboot_gates_still_hold() {
    lcd::RecoveryRebootGate gate; gate.pending = false;
    assert(!gate.allowed(true, true, false, false));
    assert(!gate.allowed(true, false, true, false));
    assert(!gate.allowed(true, false, false, true));
}
void ring_survives_reboot_and_prints_on_boot() {
    lcd::DiagStorage storage{}; lcd::DiagRing first(storage); first.wake(42);
    lcd::DiagRing reboot(storage); Serial.output.clear(); reboot.printUnread();
    assert(Serial.output.find("diag ring t=42 wake") != std::string::npos);
    Serial.output.clear(); reboot.printUnread(); assert(Serial.output.empty());
}
void ring_has_no_identifiers() {
    lcd::DiagStorage storage{}; lcd::DiagRing ring(storage);
    ring.join(1, -45, 6, true, true, true, true, -1);
    ring.failure(2, lcd::DiagStage::Connect, 5, 100, 80);
    Serial.output.clear(); ring.printUnread();
    assert(Serial.output.find("ssid") == std::string::npos);
    assert(Serial.output.find("ip=") == std::string::npos);
    assert(Serial.output.find("mac") == std::string::npos);
}
void ring_wraps_without_corruption() {
    lcd::DiagStorage storage{}; lcd::DiagRing ring(storage);
    for (unsigned i = 0; i < 200; ++i) ring.wake(i);
    Serial.output.clear(); ring.printUnread();
    assert(Serial.output.find("diag ring t=199 wake") != std::string::npos);
    assert(Serial.output.find("diag ring t=0 wake") == std::string::npos);
}

void manifest_failure_triggers_rejoin() {
    resetFakes();
    WiFi.aps = {{-45, 6, {1,2,3,4,5,6}}};
    status = HTTPC_ERROR_CONNECTION_REFUSED;
    char ids[lcd::SCREEN_SLOTS][17]{};
    lcd::RefreshResult result; CountingWatchdog watchdog;
    refresh("http://host", "lcd43b", ids, result, watchdog);
    assert(!result.manifestOk && WiFi.disconnects == 1 && WiFi.pinnedBegins == 1);
    assert(Serial.output.find("wifi rejoin reason=host-unreachable from_dbm=-50 to_dbm=-45 ok=1")
           != std::string::npos);
    assert(requestedUrls.size() == 2);   // the manifest, then once more after the re-join
    assert(requestedUrls[0] == requestedUrls[1] && requestedUrls[0].find("/manifest.json") != std::string::npos);
    // A manifest that answers (even with an error status) is not a transport failure.
    resetFakes();
    WiFi.aps = {{-45, 6, {1,2,3,4,5,6}}};
    status = 503;
    refresh("http://host", "lcd43b", ids, result, watchdog);
    assert(!result.manifestOk && WiFi.disconnects == 0 && requestedUrls.size() == 1);
}

// Socket-level helpers for the keep-alive tests: zlib frames are one range each.
struct KeepAlivePass {
    Entry art, weather, spare;
    std::vector<uint8_t> artWire, weatherWire, spareWire;
};

KeepAlivePass keepAlivePass(const char* origin = "http://host") {
    KeepAlivePass pass;
    pass.artWire = compressFrame(solidFrame(0xF800));
    pass.weatherWire = compressFrame(solidFrame(0x07E0));
    pass.spareWire = compressFrame(solidFrame(0x001F));
    pass.art = makeEntry("art", pass.artWire, lcd::FrameEncoding::Zlib);
    pass.weather = makeEntry("weather", pass.weatherWire, lcd::FrameEncoding::Zlib);
    pass.spare = makeEntry("spare", pass.spareWire, lcd::FrameEncoding::Zlib);
    // Entries carry the wire hash; the zlib frames are verified on the wire bytes.
    serveRanges(pass.art, pass.weather, pass.artWire, origin);
    urlResponses[origin + pass.weather.url] = pass.weatherWire;
    urlResponses[origin + pass.spare.url] = pass.spareWire;
    socketTestMode = true;
    return pass;
}

int (*innerResponse)() = nullptr;
std::vector<int> scriptedFailures;   // by GET index (1-based); 0 = serve normally
std::vector<bool> socketAtGet;
std::vector<std::string> countersAtGet, proofsAtGet;
int scriptedResponse() {
    const size_t call = requestedUrls.size();
    socketAtGet.push_back(socketOpen);
    const auto counter = lastHeaders.find("X-Ventanal-Counter");
    countersAtGet.push_back(counter == lastHeaders.end() ? "" : counter->second);
    const auto proof = lastHeaders.find("Authorization");
    proofsAtGet.push_back(proof == lastHeaders.end() ? "" : proof->second);
    if (call <= scriptedFailures.size() && scriptedFailures[call - 1]) return scriptedFailures[call - 1];
    return innerResponse();
}
void script(std::vector<int> failures) {
    innerResponse = fakeGetResponse;
    scriptedFailures = std::move(failures);
    socketAtGet.clear(); countersAtGet.clear(); proofsAtGet.clear();
    fakeGetResponse = scriptedResponse;
}

void pass_reuses_one_connection_for_manifest_and_ranges() {
    resetFakes();
    const auto frame = solidFrame(0xF800);
    const Entry art = makeEntry("art", frame), weather = makeEntry("weather", solidFrame(0x07E0));
    serveRanges(art, weather, frame);
    socketTestMode = true;
    char ids[lcd::SCREEN_SLOTS][17]{};
    strcpy(ids[1], weather.info.id);
    lcd::RefreshResult result; CountingWatchdog watchdog;
    refresh("http://host", "lcd43b", ids, result, watchdog);
    assert(result.manifestOk && result.frameOk[0]);
    assert(frameBytesRead == responseBody.size() + frame.size());   // manifest body + frame, each read once
    const size_t ranges = (frame.size() + kResumeChunk - 1) / kResumeChunk;
    assert(rangeStarts.size() == ranges && requestedUrls.size() == 1 + ranges);
    assert(socketConnects == 1);                    // manifest and every range on one connection
    assert(lastConnectionHeader == "keep-alive");
    assert(!socketOpen);                            // the pass closed it at the end
    assert(Serial.output.find("transport fail") == std::string::npos);
}

void reused_connection_failure_retries_fresh_once() {
    resetFakes();
    const auto pass = keepAlivePass();
    script({0, HTTPC_ERROR_CONNECTION_LOST});       // the reused connection is stale
    char ids[lcd::SCREEN_SLOTS][17]{};
    strcpy(ids[1], pass.weather.info.id);
    lcd::RefreshResult result; CountingWatchdog watchdog;
    refresh("http://host", "lcd43b", ids, result, watchdog);
    assert(result.manifestOk && result.frameOk[0]);
    assert(requestedUrls.size() == 3 && requestedUrls[1] == requestedUrls[2]);
    assert(socketAtGet[1] && !socketAtGet[2] && socketConnects == 2);   // retried on a fresh connection
    assert(Serial.output.find("transport retry=fresh-connection reason=status-5\n") != std::string::npos);

    // Both attempts fail: no third attempt.
    resetFakes();
    const auto again = keepAlivePass();
    script({0, HTTPC_ERROR_CONNECTION_LOST, HTTPC_ERROR_CONNECTION_LOST});
    refresh("http://host", "lcd43b", ids, result, watchdog);
    assert(result.manifestOk && !result.frameOk[0]);
    assert(std::count(requestedUrls.begin(), requestedUrls.end(), "http://host" + again.art.url) == 2);

    // A failure on a fresh connection is not retried (the manifest's, here).
    resetFakes();
    keepAlivePass();
    script({HTTPC_ERROR_CONNECTION_LOST});
    // (no AP is visible, so the pass keeps its association and does not refetch)
    refresh("http://host", "lcd43b", ids, result, watchdog);
    assert(!result.manifestOk && requestedUrls.size() == 1);

    // The response had started: never retried.
    resetFakes();
    keepAlivePass();
    script({0, HTTPC_ERROR_CONNECTION_LOST});
    fakeHeaderBytes = 1;
    refresh("http://host", "lcd43b", ids, result, watchdog);
    assert(result.manifestOk && !result.frameOk[0] && requestedUrls.size() == 2);
}

void connection_closed_after_unframed_body() {
    resetFakes();
    const auto pass = keepAlivePass();
    innerResponse = fakeGetResponse;
    fakeGetResponse = [] {
        const int result = innerResponse();
        if (lastUrl.find("/frames/weather-") != std::string::npos)
            fakeResponseHeaders["Connection"] = "close";     // framed, but the server closes
        if (lastUrl.find("/frames/art-") != std::string::npos)
            unframedResponse = serverClosesAfterBody = true; // body ends at the hang-up
        return result;
    };
    const char ids[lcd::SCREEN_SLOTS][17]{};
    responseBody.insert(responseBody.size() - 1, ",\"spares\":[" + pass.spare.body + "]");
    lcd::RefreshResult result; CountingWatchdog watchdog;
    refresh("http://host", "lcd43b", ids, result, watchdog);
    assert(result.manifestOk && result.frameOk[0] && result.frameOk[1] && result.frameOk[3]);
    // manifest + weather on the first connection, art on the second, spare on a third
    assert(requestedUrls.size() == 4 && socketConnects == 3);
}

void retry_uses_new_counter() {
    resetFakes();
    const auto originalInstallation = lcd::installation;
    strcpy(lcd::installation.deviceKey, std::string(64, 'a').c_str());
    strcpy(lcd::installation.pairing, "test-device");
    strcpy(lcd::installation.certificate, "fake certificate for the fake HTTP client");
    const auto pass = keepAlivePass("https://host");
    JsonDocument manifest;
    assert(!deserializeJson(manifest, responseBody));
    const auto key = decodeDeviceKey(lcd::installation.deviceKey);
    manifest["sig"] = hmacHex(key, canonicalJson(manifest.as<JsonVariantConst>()));
    responseBody.clear();
    serializeJson(manifest, responseBody);
    script({0, HTTPC_ERROR_CONNECTION_LOST});
    char ids[lcd::SCREEN_SLOTS][17]{};
    strcpy(ids[1], pass.weather.info.id);
    lcd::RefreshResult result; CountingWatchdog watchdog;
    refresh("https://host", "lcd43b", ids, result, watchdog);
    lcd::installation = originalInstallation;
    assert(result.manifestOk && result.frameOk[0] && countersAtGet.size() == 3);
    assert(std::stoull(countersAtGet[2]) > std::stoull(countersAtGet[1]));
    assert(std::stoull(countersAtGet[1]) > std::stoull(countersAtGet[0]));
    assert(!proofsAtGet[1].empty() && proofsAtGet[1] != proofsAtGet[2]);
    assert(proofsAtGet[2] == "Ventanal " + requestProof(key, "GET", pass.art.url.c_str(),
                                                        std::stoull(countersAtGet[2])));
}

void transport_fail_log_has_heap_and_no_identifiers() {
    resetFakes();
    const auto pass = keepAlivePass();
    fakeFreeInternal = 123456; fakeLargestInternal = 65536;
    script({0, HTTPC_ERROR_CONNECTION_LOST, HTTPC_ERROR_CONNECTION_REFUSED});
    char ids[lcd::SCREEN_SLOTS][17]{};
    strcpy(ids[1], pass.weather.info.id);
    lcd::RefreshResult result; CountingWatchdog watchdog;
    refresh("http://host", "lcd43b", ids, result, watchdog);
    assert(result.manifestOk && !result.frameOk[0]);
    const auto reused = Serial.output.find(
        "transport fail stage=read free_internal=123456 largest_internal=65536 reused=1\n");
    const auto fresh = Serial.output.find(
        "transport fail stage=connect free_internal=123456 largest_internal=65536 reused=0\n");
    assert(reused != std::string::npos && fresh != std::string::npos && reused < fresh);
    for (size_t at = Serial.output.find("transport "); at != std::string::npos;
         at = Serial.output.find("transport ", at + 1)) {
        const auto line = Serial.output.substr(at, Serial.output.find('\n', at) - at);
        assert(line.find("host") == std::string::npos && line.find('/') == std::string::npos);
        assert(line.find(':') == std::string::npos && line.find("lcd43b") == std::string::npos);
    }
}

void no_socket_survives_pass() {
    for (int failure : {0, HTTPC_ERROR_CONNECTION_REFUSED, HTTPC_ERROR_CONNECTION_LOST, 503}) {
        resetFakes();
        const auto pass = keepAlivePass();
        if (failure) script({0, failure});
        char ids[lcd::SCREEN_SLOTS][17]{};
        strcpy(ids[1], pass.weather.info.id);
        lcd::RefreshResult result; CountingWatchdog watchdog;
        refresh("http://host", "lcd43b", ids, result, watchdog);
        assert(fakeSocketsOpen() == 0 && socketConnects == socketCloses);
    }
    resetFakes(); keepAlivePass();
    script({HTTPC_ERROR_CONNECTION_REFUSED});
    char ids[lcd::SCREEN_SLOTS][17]{}; lcd::RefreshResult result; CountingWatchdog watchdog;
    refresh("http://host", "lcd43b", ids, result, watchdog);
    assert(fakeSocketsOpen() == 0 && socketConnects == socketCloses);
    assert(Serial.output.find("transport fail stage=connect ") != std::string::npos);
}

void sockets_stable_over_50_passes() {
    resetFakes(); const auto pass = keepAlivePass();
    char ids[lcd::SCREEN_SLOTS][17]{};
    strcpy(ids[0], pass.art.info.id); strcpy(ids[1], pass.weather.info.id);
    CountingWatchdog watchdog;
    for (int i = 0; i < 50; ++i) {
        lcd::RefreshResult result;
        refresh("http://host", "lcd43b", ids, result, watchdog);
        assert(result.manifestOk && fakeSocketsOpen() == 0);
    }
    assert(socketConnects == 50 && socketCloses == 50);
}

void rejoin_closes_open_client() {
    resetFakes(); keepAlivePass();
    WiFi.aps = {{-45, 6, {1,2,3,4,5,6}}};
    script({HTTPC_ERROR_CONNECTION_REFUSED});
    char ids[lcd::SCREEN_SLOTS][17]{}; lcd::RefreshResult result; CountingWatchdog watchdog;
    refresh("http://host", "lcd43b", ids, result, watchdog);
    assert(WiFi.disconnects == 1 && fakeSocketsOpen() == 0 && socketConnects == socketCloses);
}

void ota_stage_pass_closes_socket() {
    resetFakes(); socketTestMode = true; response = {1, 2, 3}; advertisedSize = 3;
    CountingWatchdog watchdog; const std::string key;
    const ResumeSource source{"http://host/range", "id", "", 3, true};
    {
        lcd::HostRangeTransport transport("http://host", "/range", key, watchdog, millis(), true);
        assert(transport.get(source, 0, 2, "", [](const RangeResponse&) { return true; },
            [](const uint8_t*, size_t) { return true; }));
        assert(fakeSocketsOpen() == 1);
    }
    assert(fakeSocketsOpen() == 0 && socketConnects == socketCloses);
}

void body_idle_uses_budget_up_to_15s() {
    for (bool ota : {false, true}) {
        for (uint32_t budget : {uint32_t(60000), uint32_t(9000), uint32_t(15000)}) {
            resetFakes();
            response = {1, 2, 3}; advertisedSize = 3;
            delayedFrom = 0; delayedUntilMs = 8000; holdConnection = true;
            const uint32_t started = millis();
            const uint32_t passStarted = started - (lcd::TRANSFER_BUDGET_MS - budget);
            CountingWatchdog watchdog; const std::string key;
            lcd::HostRangeTransport transport("http://host", "/range", key, watchdog, passStarted, ota);
            const ResumeSource source{"http://host/range", "id", "", 3, true};
            size_t received = 0;
            const bool complete = transport.get(source, 0, 2, "", [](const RangeResponse&) { return true; },
                [&](const uint8_t*, size_t count) { received += count; return true; });
            assert(complete && received == 3 && millis() - started >= 8000 && millis() - started < 8050);
            assert(watchdog.feeds > 0);
        }
        for (uint32_t budget : {uint32_t(60000), uint32_t(9000)}) {
            resetFakes(); response = {1, 2, 3}; advertisedSize = 3; stallBody = true;
            const uint32_t started = millis();
            CountingWatchdog watchdog; const std::string key;
            lcd::HostRangeTransport transport("http://host", "/range", key, watchdog,
                started - (lcd::TRANSFER_BUDGET_MS - budget), ota);
            const ResumeSource source{"http://host/range", "id", "", 3, true};
            assert(!transport.get(source, 0, 2, "", [](const RangeResponse&) { return true; },
                [](const uint8_t*, size_t) { return true; }));
            assert(millis() - started >= std::min(budget, uint32_t(15000)) - 20);
            assert(millis() - started <= std::min(budget, uint32_t(15000)) + 20);
        }
    }
}

void handshake_timeout_uses_remaining_budget() {
    assert(lcd::handshakeTimeoutSeconds(25000) == 20);
    assert(lcd::handshakeTimeoutSeconds(7500) == 7);
    assert(lcd::handshakeTimeoutSeconds(999) == 0);
}

void idle_connection_not_reused_after_8s() {
    resetFakes(); socketTestMode = true;
    lcd::HostConnection connection;
    auto remaining = [] { return uint32_t(60000); };
    auto prepare = [](HTTPClient&) {};
    auto feed = [] {};
    assert(connection.get("http://host", String("/one"), "", remaining, prepare, feed) > 0);
    connection.finish(true);
    delay(7990);
    assert(connection.get("http://host", String("/two"), "", remaining, prepare, feed) > 0);
    assert(connection.reused());
    connection.finish(true);
    delay(8000);
    assert(connection.get("http://host", String("/three"), "", remaining, prepare, feed) > 0);
    assert(!connection.reused());
    assert(Serial.output.find("transport reconnect reason=idle idle_ms=") != std::string::npos);
}

int delayedHostFailure() {
    if (requestedUrls.size() == 2) delay(25000);
    return HTTPC_ERROR_CONNECTION_LOST;
}

void restore_wait_bounded_by_budget() {
    assert(lcd::restoreWaitMs(1000, 1500, 10000, 20000) == 9500);
    assert(lcd::restoreWaitMs(1000, 11900, 10000, 20000) == 0);
    assert(lcd::restoreWaitMs(1000, 1500, 30000, 20000) == 20000);
    resetFakes();
    WiFi.aps = {{-45, 6, {1,2,3,4,5,6}}};
    WiFi.failPinned = true; WiFi.failedPlainBegins = 100;
    fakeGetResponse = delayedHostFailure;
    char ids[lcd::SCREEN_SLOTS][17]{}; lcd::RefreshResult result; CountingWatchdog watchdog;
    const auto started = millis();
    refresh("http://host", "lcd43b", ids, result, watchdog);
    assert(WiFi.disconnects >= 1 && WiFi.plainBegins == 2);
    assert(millis() - started <= lcd::TRANSFER_BUDGET_MS + 100);
}

int main() {
    #ifndef VENTANAL_OWNER_KEY_TEST
    body_idle_uses_budget_up_to_15s();
    #endif
    handshake_timeout_uses_remaining_budget();
    join_uses_strongest_scanned_ap();
    join_falls_back_when_bssid_join_fails();
    scan_log_has_no_identifiers();
#ifdef VENTANAL_OWNER_KEY_TEST
    static_assert(!VENTANAL_TEST_RELEASE_KEY, "owner key must not enable bench mode");
    resetFakes();
    lcd::installation = {};
    lcd::HostConnection unpaired;
    assert(!unpaired.begin("http://host", "/manifest", {}, 60000));
    strcpy(lcd::installation.deviceKey, std::string(64, 'a').c_str());
    strcpy(lcd::installation.pairing, "test-device");
    strcpy(lcd::installation.certificate, "fake certificate");
    char ids[lcd::SCREEN_SLOTS][17]{};
    lcd::RefreshResult result;
    CountingWatchdog watchdog;
    refresh("https://host", "lcd43b", ids, result, watchdog);
    assert(!requestedUrls.empty());
    assert(std::all_of(requestedUrls.begin(), requestedUrls.end(), [](const std::string& url) {
        return url.find("test_release_key=1") == std::string::npos;
    }));
    return 0;
#else
    static_assert(VENTANAL_TEST_RELEASE_KEY, "bench tests require the host test key switch");
#endif
    post_wake_failure_does_full_wifi_restart();
    non_wake_failure_keeps_plain_rejoin();
    success_clears_wake_flag();
    reboot_after_one_failed_pass_post_restart();
    post_wake_failure_retries_within_60s();
    reboot_allowed_after_proven_previous_reboot();
    reboot_blocked_when_previous_unproven();
    success_clears_pending_flag();
    reboot_gate_failed_writes_stay_blocked();
    socket_diagnostics_are_distinct();
    reboot_gates_still_hold();
    ring_survives_reboot_and_prints_on_boot();
    ring_has_no_identifiers();
    ring_wraps_without_corruption();
    no_socket_survives_pass();
    sockets_stable_over_50_passes();
    rejoin_closes_open_client();
    ota_stage_pass_closes_socket();
    idle_connection_not_reused_after_8s();
    restore_wait_bounded_by_budget();
    manifest_failure_triggers_rejoin();
    pass_reuses_one_connection_for_manifest_and_ranges();
    reused_connection_failure_retries_fresh_once();
    connection_closed_after_unframed_body();
    retry_uses_new_counter();
    transport_fail_log_has_heap_and_no_identifiers();
    rejoin_failure_restores_wifi();
    rejoin_keeps_association_when_scan_empty();
    rejoin_retries_empty_scan();
    host_failures_trigger_one_rejoin_per_pass();
    spare_larger_than_free_psram_downloads();
    two_large_frames_in_one_pass();
    memory_shortage_is_no_progress();
    streamed_jpeg_decode_matches_buffer_decode();
    streamed_zlib_decode_matches_buffer_decode();
    deferred_file_survives_replacement_and_cache_failure();
    lost_sd_source_retries_unchanged_frame_on_flash();
    recover_committed_frames_against_manifest();
    host_frame_resumes_across_passes();
    host_frame_hash_mismatch_fails();
    host_range_transport_parses_416();
    short_206_still_rejected();
    framed_206_accepted_without_close();
    {   // Oversized response: the header advertises the frame size but the
        // body keeps sending. Reads never pass the frame end, the extra
        // bytes reject the download, and the buffer is untouched beyond it.
        resetFakes();
        const std::vector<uint8_t> frame = solidFrame(0xF800);
        Entry art = makeEntry("art", frame);
        response = frame;
        response.insert(response.end(), 4096, 0xAB);   // server keeps sending
        advertisedSize = int(lcd::FRAME_BYTES);        // the header lies
        CountingWatchdog watchdog;
        std::vector<uint8_t> buffer(lcd::FRAME_BYTES + 4096, 0);   // guard band
        assert(!receiveFrame(art, buffer, watchdog, "excess"));
        assert(largestRead <= kResumeChunk);   // chunk cap held
        for (size_t i = lcd::FRAME_BYTES; i < buffer.size(); ++i)
            assert(buffer[i] == 0);                // guard band never written
    }
    {   // Delayed excess: a correct, hash-matching frame arrives, and only
        // then (300 ms later) the server sends a trailing packet. The
        // single available() check used to miss bytes that had not arrived
        // yet; connection-close framing must reject the frame and keep the
        // previous cache entry.
        resetFakes();
        const std::vector<uint8_t> frame = solidFrame(0xF800);
        const std::vector<uint8_t> previous = solidFrame(0x07E0);
        Entry art = makeEntry("art", frame);
        files["/art.vnr"] = previous;            // the cache entry to preserve
        response = frame;
        response.insert(response.end(), 4096, 0xAB);   // trailing packet...
        advertisedSize = -1;                   // no Content-Length
        delayedFrom = frame.size();              // ...arrives after 300 ms
        delayedUntilMs = 300;
        CountingWatchdog watchdog;
        std::vector<uint8_t> buffer(lcd::FRAME_BYTES + 4096, 0);   // guard band
        assert(!receiveFrame(art, buffer, watchdog, "excess"));
        assert(files["/art.vnr"] == previous);   // previous cache preserved
        for (size_t i = lcd::FRAME_BYTES; i < buffer.size(); ++i)
            assert(buffer[i] == 0);              // guard band never written
    }
    {   // Frame delivered but the server never hangs up: a connection that
        // neither delivers nor closes within the grace window cannot frame
        // the body, so the download fails closed and keeps the cache.
        resetFakes();
        const std::vector<uint8_t> frame = solidFrame(0x001F);
        const std::vector<uint8_t> previous = solidFrame(0x07E0);
        Entry art = makeEntry("art", frame);
        files["/art.vnr"] = previous;
        response = frame;
        holdConnection = true;                 // no hang-up, no extra bytes
        CountingWatchdog watchdog;
        std::vector<uint8_t> buffer(lcd::FRAME_BYTES, 0);
        advertisedSize = -1;
        assert(!receiveFrame(art, buffer, watchdog, "close-timeout"));
        assert(files["/art.vnr"] == previous);  // previous cache preserved
        advertisedSize = int(lcd::FRAME_BYTES);
        assert(receiveFrame(art, buffer, watchdog));
        assert(buffer == frame);
    }
    {   // A mid-stream disconnect rejects the partial body.
        resetFakes();
        const std::vector<uint8_t> frame = solidFrame(0x07E0);
        Entry art = makeEntry("art", frame);
        response.assign(frame.begin(), frame.begin() + 1024);   // then hangs up
        advertisedSize = int(lcd::FRAME_BYTES);
        CountingWatchdog watchdog;
        std::vector<uint8_t> buffer(lcd::FRAME_BYTES, 0);
        assert(!receiveFrame(art, buffer, watchdog, "short-body"));
        assert(!files.count("/art.vnr"));
    }
    {   // A valid frame downloads, verifies, and leaves no staging files.
        resetFakes();
        const std::vector<uint8_t> frame = solidFrame(0x001F);
        Entry art = makeEntry("art", frame);
        response = frame;
        advertisedSize = int(lcd::FRAME_BYTES);
        CountingWatchdog watchdog;
        std::vector<uint8_t> buffer(lcd::FRAME_BYTES, 0);
        assert(receiveFrame(art, buffer, watchdog));
        assert(buffer == frame);
        assert(files.empty());
    }
    {   // Verified JPEG bytes reach the staging decoder. A malformed
        // replacement fails without publishing or dropping the old art.
        std::ifstream fixture("fixtures/lcd43b-host-art.jpg", std::ios::binary);
        assert(fixture);
        std::vector<uint8_t> jpeg(std::istreambuf_iterator<char>{fixture}, {});
        assert(lcd::validJpegEnvelope(jpeg.data(), jpeg.size()));
        const auto weatherFrame = solidFrame(0x07e0);
        const Entry weather = makeEntry("weather", weatherFrame);
        char currentIds[lcd::SCREEN_SLOTS][17] = {};
        strcpy(currentIds[0], "aaaaaaaaaaaaaaaa");
        strcpy(currentIds[1], weather.info.id);
        CountingWatchdog watchdog;
        for (bool valid : {true, false}) {
            resetFakes();
            if (!valid) jpeg.pop_back();
            Entry art = makeEntry("art", jpeg, lcd::FrameEncoding::Jpeg);
            responseBody = manifestBody(art, weather);
            urlResponses["http://host" + art.url] = jpeg;
            sizeFromResponse = true;
            lcd::RefreshResult result;
            refresh("http://host", "lcd43b", currentIds, result, watchdog);
            assert(result.manifestOk && result.frameOk[0] == valid && result.frameFailed[0] == !valid);
            assert(bool(stagedBodies[0].available()) == valid && !stagedDrops[0]);
            if (valid) {
                assert(!strcmp(result.frameIds[0], art.info.id));
                assert(stagedBodies[0].size == jpeg.size());
                assert(files.at("/partials/" + stagedBodies[0].file->target) == jpeg);
                strcpy(currentIds[0], art.info.id);
            }
        }
    }
    {   // runRefresh end to end: manifest and frames arrive; solar absent
        // from the manifest stages a drop; a second pass skips downloads.
        resetFakes();
        const std::vector<uint8_t> artFrame = solidFrame(0xF800);
        const std::vector<uint8_t> weatherFrame = solidFrame(0x07E0);
        Entry art = makeEntry("art", artFrame);
        Entry weather = makeEntry("weather", weatherFrame);
        responseBody = manifestBody(art, weather);
        urlResponses["http://host/devices/lcd43b/manifest.json?board=lcd43b&fw=1.0.0&test_release_key=1"] =
            std::vector<uint8_t>(responseBody.begin(), responseBody.end());
        urlResponses["http://host" + art.url] = artFrame;
        urlResponses["http://host" + weather.url] = weatherFrame;
        sizeFromResponse = true;
        char currentIds[lcd::SCREEN_SLOTS][17] = {};
        strcpy(currentIds[2], "cccccccccccccccc");
        files["/solar.vnr"] = std::vector<uint8_t>(64, 0x5A);   // persisted fallback
        lcd::RefreshResult result;
        CountingWatchdog watchdog;
        refresh("http://host", "lcd43b", currentIds, result, watchdog);
        assert(result.manifestOk);
        assert(result.frameOk[0] && result.frameOk[1]);
        assert(!result.frameOk[2]);
        assert(result.dropSlot[2]);       // solar vanished from the manifest
        for (int slot = 3; slot < lcd::SCREEN_SLOTS; ++slot)
            assert(result.dropSlot[slot]);   // unpublished spare slots
        assert(stagedBodies[0].available() && stagedBodies[1].available());
        assert(stagedDrops[2] && files.size() == 3 && files.count("/solar.vnr"));
        assert(!strcmp(result.frameIds[0], art.info.id));
        assert(!strcmp(result.frameIds[1], weather.info.id));
        assert(!lcd::anyRequiredFrameFailed(result));   // nothing required failed

        // Second pass with the live ids: both slots are skipped and
        // stage their current ids (no new downloads at all).
        for (int slot = 0; slot < lcd::SCREEN_SLOTS; ++slot)
            strlcpy(currentIds[slot], result.frameIds[slot], 17);
        frameReadCalls = 0;
        lcd::RefreshResult again;
        refresh("http://host", "lcd43b", currentIds, again, watchdog);
        assert(again.manifestOk);
        assert(again.frameOk[0] && again.frameOk[1]);
        assert(!strcmp(again.frameIds[0], currentIds[0]));
        assert(!strcmp(again.frameIds[1], currentIds[1]));
        assert(frameReadCalls == 0);      // nothing re-downloaded
        assert(strstr(Serial.output.c_str(), "skipped=unchanged"));
        assert(!lcd::anyRequiredFrameFailed(again));   // nothing was required

        // A frame the scan-out owner cannot replace stays unacknowledged.
        rejectStageSlot = 1;
        currentIds[1][0] = '\0';
        lcd::RefreshResult rejected;
        refresh("http://host", "lcd43b", currentIds, rejected, watchdog);
        assert(rejected.manifestOk && rejected.frameOk[0]);
        assert(!rejected.frameOk[1] && !rejected.frameFailed[1] && !rejected.frameIds[1][0]);
        assert(rejected.memoryOnlyPending);
        assert(files.count("/partials/" + stagedBodies[1].file->target));
    }
    {   // Hidden Art and spares are never requested; cached hidden files stay.
        resetFakes();
        auto frame=solidFrame(0xffff);
        Entry art=makeEntry("art",frame), weather=makeEntry("weather",frame), spare=makeEntry("spare0",frame);
        responseBody=manifestBody(art,weather);
        responseBody.insert(responseBody.size()-1,",\"spares\":["+spare.body+"]");
        urlResponses["http://host"+weather.url]=frame;
        sizeFromResponse=true;
        files["/art.vnr"]={1,2,3}; files["/spare0.vnr"]={4,5,6};
        char ids[lcd::SCREEN_SLOTS][17]={};
        auto settings=lcd::defaultSettings(false); settings.showArt=false;
        lcd::RefreshResult result; CountingWatchdog watchdog;
        refresh("http://host","lcd43b",ids,result,watchdog,settings);
        assert(result.manifestOk && result.frameOk[1] && !lcd::anyRequiredFrameFailed(result));
        assert(!result.frameOk[0] && !result.frameOk[3] && !result.dropSlot[0] && !result.dropSlot[3]);
        assert(files["/art.vnr"]==std::vector<uint8_t>({1,2,3}));
        assert(files["/spare0.vnr"]==std::vector<uint8_t>({4,5,6}));
        assert(requestedUrls.size()==2 && requestedUrls[1]=="http://host"+weather.url);
        // If Solar alone was enabled but is no longer published, download
        // all published fallback screens so the board cannot get stuck.
        requestedUrls.clear(); settings.showWeather=false;
        urlResponses["http://host"+art.url]=frame;
        urlResponses["http://host"+spare.url]=frame;
        refresh("http://host","lcd43b",ids,result,watchdog,settings);
        assert(!result.frameOk[0] && result.frameOk[1] && !result.frameOk[3]);
        // The previous pass left a verified weather stage while the cache write was deferred.
        assert(requestedUrls.size() == 1 && result.frameOk[1]);
    }
    {   // A compressed body is verified and decoded before publication.
        // Cache persistence receives the original wire bytes unchanged.
        resetFakes();
        const auto raw = solidFrame(0xF800);
        const auto zipped = compressFrame(raw);
        auto badRaw = raw;
        badRaw[8] = 1; // recompress and hash a malformed frame to test decoding
        const auto bad = compressFrame(badRaw);
        const Entry art = makeEntry("art", zipped, lcd::FrameEncoding::Zlib);
        const Entry weather = makeEntry("weather", bad, lcd::FrameEncoding::Zlib);
        responseBody = manifestBody(art, weather);
        urlResponses["http://host" + art.url] = zipped;
        urlResponses["http://host" + weather.url] = bad;
        sizeFromResponse = true;
        char ids[lcd::SCREEN_SLOTS][17]{};
        CountingWatchdog watchdog;
        lcd::RefreshResult result;
        refresh("http://host", "lcd43b", ids, result, watchdog);
        assert(result.frameOk[0] && result.frameFailed[1]);
        assert(!stagedBodies[1].available() && files.size() == 1);
        assert(Serial.output.find("frame slot=weather outcome=failed reason=invalid-frame ")
               != std::string::npos);
        const auto& body = stagedBodies[0];
        assert(body.size == zipped.size());
        assert(files.at("/partials/" + body.file->target) == zipped);

        files["/art.vnr"] = raw;
        failRename = true;
        assert(!lcd::writeCachedFrame("/art.vnr", body));
        assert(files.size() == 2 && files.at("/art.vnr") == raw);
        failRename = false;
        assert(lcd::writeCachedFrame("/art.vnr", body));
        assert(files.size() == 2 && files.at("/art.vnr") == zipped);
        assert(lcd::copyCachedFrame("/override.vnr", "/art.vnr"));
        assert(files.at("/override.vnr") == zipped);
    }
    {   // The confirmed NVS pointer controls which bank can be deleted.
        files.clear();
        preferences.clear();
        Preferences prefs;
        assert(prefs.begin("ventanal", false));
        lcd::CacheLocation current{"aaaaaaaaaaaaaaaa", false};
        assert(lcd::saveCacheLocation(prefs, 0, current));
        files["/art.vnr"] = {1};
        const auto bytes = std::make_shared<uint8_t>(2);
        lcd::DeferredSave save;
        assert(save.replace("bbbbbbbbbbbbbbbb", {bytes, 1}, 0));

        Preferences::failStringWrite = true;
        assert(!lcd::persistCacheSlot(prefs, 0, current, save));
        assert(!current.alternate && prefs.getString("id0") == "aaaaaaaaaaaaaaaa");
        assert(files.at("/art.vnr")[0] == 1 && files.at("/art-alt.vnr")[0] == 2);
        Preferences::failStringWrite = false;
        assert(lcd::persistCacheSlot(prefs, 0, current, save));
        assert(current.alternate && !files.count("/art.vnr") && files.count("/art-alt.vnr"));

        assert(save.replace("cccccccccccccccc", {bytes, 1}, 1));
        assert(lcd::persistCacheSlot(prefs, 0, current, save));
        assert(!current.alternate && files.count("/art.vnr") && !files.count("/art-alt.vnr"));
        files["/art-alt.vnr"] = {3}; // orphaned inactive bank
        assert(save.replace("", {}, 2));
        Preferences::failStringWrite = true;
        assert(!lcd::persistCacheSlot(prefs, 0, current, save));
        assert(current.id[0] && files.size() == 2);
        Preferences::failStringWrite = false;
        assert(lcd::persistCacheSlot(prefs, 0, current, save));
        assert(!current.id[0] && files.empty());
        lcd::CacheLocation loaded[lcd::SCREEN_SLOTS];
        lcd::loadCacheLocations(prefs, loaded);
        assert(!loaded[0].id[0]);
    }
    {   // Reap scratch files without touching committed files.
        files = {{"/weather.vnr.tmp", {1}}, {"/override.vnr.tmp", {2}},
                 {"/art.vnr", {3}}, {"/keep.tmp.backup", {4}}};
        lcd::sweepCacheTemps();
        assert(files.size() == 2);
        assert(files.at("/art.vnr")[0] == 3 && files.at("/keep.tmp.backup")[0] == 4);
    }
    {   // Bounds and framing apply independently of the hash. ASan and the
        // guard bytes cover writes past the exact decoded image length.
        auto raw = solidFrame(0x07E0);
        const auto valid = compressFrame(raw);
        std::vector<uint8_t> output(lcd::FRAME_BYTES + 16, 0xAD);
        const auto decode = [&](const std::vector<uint8_t>& data) {
            const bool ok = lcd::decodeFrame(lcd::FrameEncoding::Zlib,
                                             data.data(), data.size(), output.data());
            for (size_t i = lcd::FRAME_BYTES; i < output.size(); ++i)
                assert(output[i] == 0xAD);
            return ok;
        };
        assert(decode(valid));
        assert(std::equal(raw.begin(), raw.end(), output.begin()));
        auto bad = valid;
        bad.back() ^= 1;                       // corrupt Adler-32
        assert(!decode(bad));
        bad = valid;
        bad.pop_back();                        // truncated stream
        assert(!decode(bad));
        for (size_t cut : {size_t(1), size_t(8), valid.size() / 2})
            assert(!decode(std::vector<uint8_t>(valid.begin(), valid.begin() + cut)));
        bad = valid;
        bad.push_back(0);                      // excess wire byte
        assert(!decode(bad));
        assert(!decode(std::vector<uint8_t>(lcd::FRAME_WIRE_MAX + 1)));
        raw.push_back(0);                      // expansion exceeds capacity
        assert(!decode(compressFrame(raw)));
        raw.resize(lcd::FRAME_BYTES - 1);      // expansion too short
        assert(!decode(compressFrame(raw)));
        raw.resize(lcd::FRAME_BYTES);
        raw[6] = 0;                            // wrong dimensions
        assert(!decode(compressFrame(raw)));
    }
    {   // Stalled transfers with the watchdog enabled: the manifest itself
        // arrives, every frame transfer gives up after its budget, the
        // whole pass returns bounded, and the watchdog is fed throughout
        // the stall (the worker stays alive and supervised).
        resetFakes();
        const std::vector<uint8_t> artFrame = solidFrame(0xF800);
        const std::vector<uint8_t> weatherFrame = solidFrame(0x07E0);
        Entry art = makeEntry("art", artFrame);
        Entry weather = makeEntry("weather", weatherFrame);
        responseBody = manifestBody(art, weather);
        urlResponses["http://host/devices/lcd43b/manifest.json?board=lcd43b&fw=1.0.0&test_release_key=1"] =
            std::vector<uint8_t>(responseBody.begin(), responseBody.end());
        // Full-length bodies so the advertised size passes the gate; the
        // stall flag keeps any body byte from ever arriving.
        urlResponses["http://host" + art.url] = artFrame;
        urlResponses["http://host" + weather.url] = weatherFrame;
        sizeFromResponse = true;
        stallBody = true;                // frames never deliver a byte
        char currentIds[lcd::SCREEN_SLOTS][17] = {};
        lcd::RefreshResult result;
        CountingWatchdog watchdog;
        const uint32_t started = fakeClock;
        refresh("http://host", "lcd43b", currentIds, result, watchdog);
        assert(result.manifestOk);        // the manifest itself arrived
        assert(!result.frameOk[0] && !result.frameOk[1]);
        assert(!result.dropSlot[0] && !result.dropSlot[1]);
        assert(!result.frameFailed[0] && !result.frameFailed[1]);   // awaiting more data
        assert(!lcd::anyRequiredFrameFailed(result));
        assert(!stagedBodies[0].available() && !stagedBodies[1].available());
        const uint32_t elapsed = fakeClock - started;
        assert(elapsed < lcd::TRANSFER_BUDGET_MS);
        assert(watchdog.feeds > 1000);   // fed during both frames
        for (const char* slot : {"weather", "art"}) {
            const auto expected = std::string("frame slot=") + slot
                + " outcome=no-progress reason=body-idle got=0 have=0";
            assert(Serial.output.find(expected) != std::string::npos);
        }
        assert(std::count(requestedUrls.begin(), requestedUrls.end(), "http://host" + weather.url) == 1);
    }
    {   // Keep-alive (hardware fix 8): a persistent HTTP/1.1 server keeps the
        // socket open after the body, and a framed body (Content-Length)
        // completes without waiting for a hang-up (round-4 Important 1 was
        // the unframed case, which still waits for the close).
        resetFakes();
        const std::vector<uint8_t> frame = solidFrame(0x001F);
        Entry art = makeEntry("art", frame);
        response = frame;
        advertisedSize = int(lcd::FRAME_BYTES);
        persistentServer = true;      // stays open only under keep-alive
        CountingWatchdog watchdog;
        std::vector<uint8_t> buffer(lcd::FRAME_BYTES, 0);
        assert(receiveFrame(art, buffer, watchdog));
        assert(lastConnectionHeader == "keep-alive");   // one connection per pass
        assert(buffer == frame && files.empty());
    }
    {   // Manifest ok but a required frame fails: the failure is tracked per
        // slot, separately from manifest success (round-4 Important 2 used
        // to show UP TO DATE over stale artwork). Partial success across
        // slots: art is staged while weather's answer has the wrong length.
        resetFakes();
        const std::vector<uint8_t> artFrame = solidFrame(0xF800);
        const std::vector<uint8_t> weatherFrame = solidFrame(0x07E0);
        Entry art = makeEntry("art", artFrame);
        Entry weather = makeEntry("weather", weatherFrame);
        responseBody = manifestBody(art, weather);
        urlResponses["http://host/devices/lcd43b/manifest.json?board=lcd43b&fw=1.0.0&test_release_key=1"] =
            std::vector<uint8_t>(responseBody.begin(), responseBody.end());
        urlResponses["http://host" + art.url] = artFrame;
        // Weather's response advertises a different size from the manifest.
        urlResponses["http://host" + weather.url] =
            std::vector<uint8_t>(1024, 0);
        sizeFromResponse = true;
        char currentIds[lcd::SCREEN_SLOTS][17] = {};
        lcd::RefreshResult result;
        CountingWatchdog watchdog;
        refresh("http://host", "lcd43b", currentIds, result, watchdog);
        assert(result.manifestOk);                 // the manifest itself arrived
        assert(result.frameOk[0] && !result.frameOk[1]);
        assert(!result.frameFailed[0]);            // art: required and done
        assert(result.frameFailed[1]);            // weather: required, failed
        assert(!result.frameFailed[2] && result.dropSlot[2]);   // drop != fail
        assert(lcd::anyRequiredFrameFailed(result));   // pass is a failure
        assert(Serial.output.find("frame slot=weather outcome=failed reason=size got=0 have=0")
               != std::string::npos);
        assert(stagedBodies[0].available() && !stagedBodies[1].available() && files.size() == 1);
        assert(lastConnectionHeader == "keep-alive");  // requests share the pass connection
    }
    {   // The shared queue wire format is sent on the device manifest GET.
        resetFakes();
        const auto frame=solidFrame(0xffff);
        const auto art=makeEntry("art",frame), weather=makeEntry("weather",frame);
        JsonDocument manifest;
        assert(!deserializeJson(manifest, manifestBody(art, weather)));
        manifest["firmware"]["release"] = "test metadata";
        manifest["firmware"]["sig"] = "test signature";
        manifest["firmware"]["path"] = "/firmware/lcd43b-3.0.0.bin";
        manifest["firmware"]["approval"] = "0123456789abcdef";
        serializeJson(manifest, responseBody);
        sizeFromResponse=true;
        char ids[lcd::SCREEN_SLOTS][17]={};
        lcd::RefreshResult result; CountingWatchdog watchdog;
        refresh("http://host/","lcd43b",ids,result,watchdog,lcd::DeviceSettings{},
                "aaaaaaaaaaaaaaaa~bbbbbbbbbbbbbbbb", "rolled_back:2.0.0", "fedcba9876543210");
        assert(result.manifestOk);
        assert(!strcmp(result.firmware.path, "/firmware/lcd43b-3.0.0.bin"));
        assert(!strcmp(result.firmware.approval, "0123456789abcdef"));
        assert(lastUserAgent == "Ventanal/1.0.0");
        assert(requestedUrls[0]=="http://host/devices/lcd43b/manifest.json?board=lcd43b&fw=1.0.0&test_release_key=1&lcd_diag=0,0,0,0"
                                "&skips=aaaaaaaaaaaaaaaa~bbbbbbbbbbbbbbbb"
                                "&ota=rolled_back:2.0.0&ota_approval=fedcba9876543210");
    }
    {   // Manifest failure: nothing staged, nothing downloaded.
        resetFakes();
        status = 500;
        advertisedSize = 0;
        char currentIds[lcd::SCREEN_SLOTS][17] = {};
        lcd::RefreshResult result;
        CountingWatchdog watchdog;
        refresh("http://host", "lcd43b", currentIds, result, watchdog);
        assert(!result.manifestOk);
        for (int slot = 0; slot < lcd::SCREEN_SLOTS; ++slot)
            assert(!result.frameOk[slot] && !result.dropSlot[slot]);
        // A failed manifest is offline feedback, not a frame failure.
        assert(!lcd::anyRequiredFrameFailed(result));
    }
    {   // applyRefresh: state transitions and the redraw decision.
        Preferences prefs;
        assert(prefs.begin("ventanal", false));
        lcd::DeviceManifest published;
        published.quietStart = 1380;
        published.quietEnd = 360;
        published.spareCount = 1;
        lcd::RefreshResult result;
        result.manifestOk = true;
        result.manifest = published;
        strcpy(result.frameIds[0], "aaaaaaaaaaaaaaaa");
        result.frameOk[0] = true;

        lcd::RefreshState state;         // a valid frame is already shown
        state.offline = false;
        state.drawn = true;
        strcpy(state.cachedIds[0], "bbbbbbbbbbbbbbbb");
        int screenIndex = 0, spareCursor = 0;
        bool flashTouched = false;
        assert(lcd::applyRefresh(state, screenIndex, spareCursor, prefs,
                                 result, false, {}, &flashTouched));
        assert(flashTouched);
        assert(lcd::applyRefresh(state, screenIndex, spareCursor, prefs,
                                 result, false, {}, &flashTouched));
        assert(!flashTouched);
        assert(!state.offline);
        assert(!strcmp(state.cachedIds[0], "bbbbbbbbbbbbbbbb")); // only publication changes live ids
        assert(!prefs.isKey("id0"));                           // only deferred save writes cache ids
        assert(state.manifest.spareCount == 1);
        assert(lcd::loadQuiet(prefs, state.manifest));
        assert(state.manifest.quietStart == 1380);

        // A failed scheduled refresh after a successful display must
        // redraw (the OFFLINE mark changed) — pinned by the review.
        lcd::RefreshResult failed;
        assert(lcd::applyRefresh(state, screenIndex, spareCursor, prefs,
                                 failed, false));
        assert(state.offline);

        // A failed refresh while already offline changes nothing visible.
        assert(!lcd::applyRefresh(state, screenIndex, spareCursor, prefs,
                                  failed, false));

        // A shrunk screen list normalises the RAM selection for deferred persistence.
        published.spareCount = 0;
        lcd::RefreshResult shrunk;
        shrunk.manifestOk = true;
        shrunk.manifest = published;
        screenIndex = 4;
        spareCursor = 0;
        lcd::applyRefresh(state, screenIndex, spareCursor, prefs, shrunk,
                          false);
        assert(screenIndex == 0);        // healed to art
        int stored = 0, cursor = 0;
        assert(!lcd::loadSelection(prefs, stored, cursor));
    }
    {   // redrawNeeded truth table (host-pinned): new content, first draw,
        // offline change or an explicit request each force a redraw.
        assert(lcd::redrawNeeded(false, false, false, false));   // first draw
        assert(lcd::redrawNeeded(false, false, true, true));    // offline flip
        assert(lcd::redrawNeeded(false, true, true, false));    // new content
        assert(lcd::redrawNeeded(true, false, true, false));    // requested
        assert(!lcd::redrawNeeded(false, false, true, false));  // nothing new
    }
    {   // Offline reboot with retained RTC time: the timezone is
        // initialised in setup (connectivity-independent), so the persisted
        // LOCAL quiet window evaluates in the configured zone, not UTC.
        resetFakes();
        Preferences prefs;
        assert(prefs.begin("ventanal", false));
        lcd::DeviceManifest published;         // what the last online sync stored
        published.quietStart = 1380;            // 23:00 local
        published.quietEnd = 240;               // 04:00 local
        lcd::saveQuiet(prefs, published);
        lcd::DeviceManifest rebooted;           // fresh boot: NVS only, no network
        assert(lcd::loadQuiet(prefs, rebooted));
        // setup() initialises the configured timezone before any refresh.
        lcd::configureTimezone();
        assert(fakeTz == VENTANAL_TZ);
        // Retained RTC: 2026-11-17 04:30 UTC, synced before the reboot.
        std::tm utc{};
        utc.tm_year = 2026 - 1900;
        utc.tm_mon = 10;
        utc.tm_mday = 17;
        utc.tm_hour = 4;
        utc.tm_min = 30;
        const time_t instant = timegm(&utc);
        setenv("TZ", fakeTz.c_str(), 1);       // EST (UTC-5) in November
        tzset();
        std::tm local{};
        localtime_r(&instant, &local);
        const int localMinutes = local.tm_hour * 60 + local.tm_min;
        assert(localMinutes == 23 * 60 + 30);  // 23:30 local, inside the window
        assert(lcd::quietApplies(local.tm_year + 1900, localMinutes, rebooted));
        // The old bug — timezone configured only after Wi-Fi connected, so
        // an offline reboot evaluated the LOCAL window in UTC — keeps the
        // panel lit through the night.
        unsetenv("TZ");
        tzset();
        std::tm utcTm{};
        gmtime_r(&instant, &utcTm);
        assert(!lcd::quietApplies(utcTm.tm_year + 1900,
                                   utcTm.tm_hour * 60 + utcTm.tm_min, rebooted));
    }
    puts("lcd43b sync: bounded reads, excess rejection, stall budgets,"
         " watchdog feeds, staged results, redraw rules passed");
    return 0;
}
