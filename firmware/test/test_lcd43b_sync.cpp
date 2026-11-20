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
#include "../lcd43b/inflate.h"
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
bool jpegPixelsChecked = false;
int rejectStageSlot = -1;
unsigned wireReclaims = 0;

void refresh(const char* host, const char* device, const char ids[lcd::SCREEN_SLOTS][17],
             lcd::RefreshResult& result, CountingWatchdog& watchdog,
             const lcd::DeviceSettings& settings = {}, const char* skips = "",
             const char* ota = "", const char* approval = "",
             void (*reclaim)(size_t) = nullptr, const char (*persisted)[17] = nullptr) {
    std::vector<uint8_t> staging(lcd::FRAME_BYTES);
    lcd::ResidentCache cache;
    cache.staging = staging.data();
    cache.reclaimWireMemory = reclaim;
    if (persisted) memcpy(cache.persistedIds, persisted, sizeof cache.persistedIds);
    std::fill_n(cache.allocated, lcd::SCREEN_SLOTS, true);
    cache.publish = [](int slot, const lcd::FrameInfo* info, const lcd::FrameBody& body) {
        if (info && slot == rejectStageSlot) return false;
        if (!info) stagedDrops[slot] = true;
        else stagedBodies[slot] = body;
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
    const auto download =
        lcd::receiveFrame("http://host", entry.info, "art", buffer.data(), watchdog, millis());
    if (reason) assert(download.reason && !strcmp(download.reason, reason));
    return download.outcome == FetchOutcome::Complete;
}

std::string manifestBody(const Entry& art, const Entry& weather) {
    return "{\"version\":1,\"screens\":{\"art\":" + art.body
         + ",\"weather\":" + weather.body
         + "},\"quiet\":{\"start\":\"23:00\",\"end\":\"06:00\"}}";
}

void resetFakes() {
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
    failRename = false;
    std::fill_n(stagedBodies, lcd::SCREEN_SLOTS, lcd::FrameBody{});
    std::fill_n(stagedDrops, lcd::SCREEN_SLOTS, false);
    rejectStageSlot = -1;
    fakeTz.clear();
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
    assert(!stagedBodies[0].bytes && files.at("/art.vnr") == previous);
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
    rangeStarts.clear();
    Serial.output.clear();
    strcpy(ids[3], spare.info.id);
    refresh("http://host", "lcd43b", ids, result, watchdog);
    assert(!rangeStarts.empty() && rangeStarts.front() == kResumeChunk);
    assert(frameBytesRead == frame.size());
    assert(result.frameOk[0] && !result.frameFailed[0] && !strcmp(result.frameIds[0], art.info.id));
    assert(stagedBodies[0].size == frame.size());
    assert(memcmp(stagedBodies[0].bytes.get(), frame.data(), frame.size()) == 0);
    assert(files.size() == 1 && files.at("/art.vnr") == previous);
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
    assert(!result.frameOk[0] && result.frameFailed[0] && !stagedBodies[0].bytes);
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

// The ESP32-S3 ROM is absent on the host. Supply the RGB888 MCU blocks it
// would emit for the solid-red JPEG to exercise the real staging writer.
bool lcd::decodeHostJpeg(const uint8_t* data, size_t size, uint8_t* frame, Watchdog& watchdog) {
    if (!validJpegEnvelope(data, size)) return false;
    uint8_t rgb[16 * 16 * 3]{};
    for (size_t i = 0; i < sizeof rgb; i += 3) rgb[i] = 248;
    for (unsigned y = 0; y < HEIGHT; y += 16) {
        watchdog.feed();
        for (unsigned x = 0; x < WIDTH; x += 16)
            assert(writeJpegBlock(frame, rgb, x, y, 16, 16));
    }
    assert(!writeJpegBlock(frame, rgb, WIDTH - 1, HEIGHT - 1, 2, 1));
    for (size_t i = HEADER_BYTES; i < FRAME_BYTES; i += 2)
        assert(frame[i] == 0 && frame[i + 1] == 0xf8);
    jpegPixelsChecked = true;
    return true;
}

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
    assert(files.count("/partials/frame-art") && !files.count("/partials/frame-weather"));
    memcpy(ids, result.frameIds, sizeof ids);
    refresh("http://host", "lcd43b", ids, result, watchdog, {}, "", "", "", nullptr, ids);
    assert(result.frameOk[0] && !files.count("/partials/frame-art"));
}

int main() {
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
            jpegPixelsChecked = false;
            lcd::RefreshResult result;
            refresh("http://host", "lcd43b", currentIds, result, watchdog);
            assert(result.manifestOk && result.frameOk[0] == valid && result.frameFailed[0] == !valid);
            assert(jpegPixelsChecked == valid);
            assert(bool(stagedBodies[0].bytes) == valid && !stagedDrops[0]);
            if (valid) {
                assert(!strcmp(result.frameIds[0], art.info.id));
                assert(stagedBodies[0].size == jpeg.size());
                assert(!memcmp(stagedBodies[0].bytes.get(), jpeg.data(), jpeg.size()));
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
        assert(stagedBodies[0].bytes && stagedBodies[1].bytes);
        assert(stagedDrops[2] && files.size() == 1 && files.count("/solar.vnr"));
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
        assert(!rejected.frameOk[1] && rejected.frameFailed[1] && !rejected.frameIds[1][0]);
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
        assert(requestedUrls.size()==2);
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
        assert(!stagedBodies[1].bytes && files.empty());
        assert(Serial.output.find("frame slot=weather outcome=failed reason=invalid-frame ")
               != std::string::npos);
        const auto& body = stagedBodies[0];
        assert(body.size == zipped.size());
        assert(memcmp(body.bytes.get(), zipped.data(), zipped.size()) == 0);

        files["/art.vnr"] = raw;
        failRename = true;
        assert(!lcd::writeCachedFrame("/art.vnr", body));
        assert(files.size() == 1 && files.at("/art.vnr") == raw);
        failRename = false;
        assert(lcd::writeCachedFrame("/art.vnr", body));
        assert(files.size() == 1 && files.at("/art.vnr") == zipped);
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
        assert(!stagedBodies[0].bytes && !stagedBodies[1].bytes);
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
    {   // Connection-close framing must actually ask for closure: the
        // outgoing request carries Connection: close (keep-alive disabled),
        // so a persistent-capable HTTP/1.1 server — one that keeps the
        // socket open after the body under keep-alive — hangs up and the
        // frame verifies (round-4 Important 1: the default keep-alive made
        // every such download time out on the close grace).
        resetFakes();
        const std::vector<uint8_t> frame = solidFrame(0x001F);
        Entry art = makeEntry("art", frame);
        response = frame;
        advertisedSize = int(lcd::FRAME_BYTES);
        persistentServer = true;      // stays open only under keep-alive
        CountingWatchdog watchdog;
        std::vector<uint8_t> buffer(lcd::FRAME_BYTES, 0);
        assert(receiveFrame(art, buffer, watchdog));
        assert(lastConnectionHeader == "close");   // asked the server to close
        assert(!reuse);                            // keep-alive disabled
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
        assert(stagedBodies[0].bytes && !stagedBodies[1].bytes && files.empty());
        assert(lastConnectionHeader == "close");  // every request asks closure
    }
    {   // Allocation failure: every slot the manifest required this pass
        // stages a frame failure, so the pass reports failure (not the
        // UP TO DATE that a manifest-only success used to show).
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
        failAllocation = true;                  // no allocation for the wire bytes
        char currentIds[lcd::SCREEN_SLOTS][17] = {};
        lcd::RefreshResult result;
        CountingWatchdog watchdog;
        refresh("http://host", "lcd43b", currentIds, result, watchdog);
        assert(result.manifestOk);
        assert(result.frameFailed[0] && result.frameFailed[1]);   // required
        assert(lcd::anyRequiredFrameFailed(result));
        assert(!stagedBodies[0].bytes && !stagedBodies[1].bytes);

        wireReclaims = 0;
        refresh("http://host", "lcd43b", currentIds, result, watchdog, {}, "", "", "",
                [](size_t bytes) {
                    assert(bytes == lcd::FRAME_BYTES && failAllocation);
                    ++wireReclaims;
                    failAllocation = false;
                });
        assert(wireReclaims == 1);
        assert(result.manifestOk && !lcd::anyRequiredFrameFailed(result));
        assert(stagedBodies[0].bytes && stagedBodies[1].bytes);
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
