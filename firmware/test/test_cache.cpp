// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
#include "../ventanal/cache.h"
#include "../ventanal/net.h"
#include "../ventanal/security.h"
#include "../common/resume/storage.h"
#include <LittleFS.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <esp_heap_caps.h>
#include <cassert>
#include <iostream>
#include <chrono>
#include <map>
#include <zlib.h>
#include "../lcd43b/src/miniz/ventanal_tinfl.h"

namespace {
const std::string url = std::string(installation.host) + "/frame";
constexpr char cachePath[] = "/weather.vnf";
constexpr char partialPath[] = "/partials/frame-weather.vnf.tmp.part";
constexpr size_t chunkSize = 32768;
std::vector<uint8_t> frame;
std::vector<size_t> offsets;
size_t stopBefore = 0;
bool slowLink = false;
std::map<std::string, std::vector<uint8_t>> bodies; // Per-URL wire bodies.

const std::vector<uint8_t>& servedBody() {
    const auto found = bodies.find(lastUrl);
    return found == bodies.end() ? frame : found->second;
}

int rangedResponse() {
    const auto& body = servedBody();
    const auto range = lastHeaders.find("Range");
    if (range == lastHeaders.end()) return 503;
    unsigned start, end;
    assert(sscanf(range->second.c_str(), "bytes=%u-%u", &start, &end) == 2);
    offsets.push_back(start);
    if (stopBefore && start >= stopBefore) {
        delay(30000);
        return HTTPC_ERROR_READ_TIMEOUT;
    }
    assert(end < body.size() && end - start + 1 <= chunkSize);
    // Model 256 kbit/s of payload throughput plus a 300 ms request round trip.
    if (slowLink) delay(300 + (end - start + 32) / 32);
    response.assign(body.begin()+start, body.begin()+end+1);
    advertisedSize = response.size();
    fakeResponseHeaders["Content-Range"] = "bytes " + std::to_string(start) + "-" +
        std::to_string(end) + "/" + std::to_string(body.size());
    fakeResponseHeaders["ETag"] = "\"0123456789abcdef\"";
    if (start) assert(lastHeaders["If-Range"] == fakeResponseHeaders["ETag"]);
    return 206;
}

void prepareRanges() {
    offsets.clear();
    FrameHash hash;
    hash.add(frame.data(), frame.size());
    expectedHashes = {{"/frame", hash.finish()}};
    fakeGetResponse = rangedResponse;
}

#if VENTANAL_ABORT_AFTER_BYTES > 0
void epaper_abort_after_bytes_still_aborts() {
    prepareRanges();
    const auto before = frameBytesRead;
    const auto good = files.at(cachePath);
    assert(!fetchFrame(url.c_str(), cachePath));
    assert(frameBytesRead - before == VENTANAL_ABORT_AFTER_BYTES);
    assert(files.at(cachePath) == good);
    assert(files.at(partialPath).size() == 3 * chunkSize);
}
#else
const ScreenInfo weather{"0123456789abcdef", "/frame", {}};
const ManifestFrameSlots manifestFrames{nullptr, &weather};

constexpr char wirePartial[] = "/partials/frame-weather.vnz.tmp.part";
constexpr char wireStage[] = "/partials/frame-weather.vnz.tmp";

std::string digestOf(const std::vector<uint8_t>& bytes) {
    FrameHash hash;
    hash.add(bytes.data(), bytes.size());
    return hash.finish();
}

std::vector<uint8_t> deflated(const std::vector<uint8_t>& raw) {
    uLongf size = compressBound(raw.size());
    std::vector<uint8_t> wire(size);
    assert(compress2(wire.data(), &size, raw.data(), raw.size(), 9) == Z_OK);
    wire.resize(size);
    return wire;
}

// A valid VNF1 frame whose first `noisy` pixel bytes are random panel indices.
std::vector<uint8_t> panelFrame(size_t noisy, unsigned seed) {
    std::vector<uint8_t> pixels(frame.begin(), frame.begin() + 16);
    pixels.resize(960016, 0x23);
    uint32_t random = seed * 2654435761u + 1;  // Small LCG: test stack frames stay < 2 KiB.
    const auto next = [&] { random = random * 1664525u + 1013904223u; return (random >> 16) % 6; };
    for (size_t i = 16; i < 16 + noisy && i < pixels.size(); ++i)
        pixels[i] = uint8_t(next() << 4 | next());
    return pixels;
}

// The smallest noisy frame whose zlib wire size reaches `wireBytes`.
std::vector<uint8_t> frameWithWireSize(size_t wireBytes, unsigned seed) {
    size_t low = 0, high = 960000;
    while (low < high) {
        const size_t middle = (low + high) / 2;
        if (deflated(panelFrame(middle, seed)).size() < wireBytes) low = middle + 1;
        else high = middle;
    }
    return panelFrame(low, seed);
}

ExpectedHash zlibExpected(const char* path, const std::vector<uint8_t>& decoded,
                          const std::vector<uint8_t>& wire, const char* sha = nullptr) {
    const auto digest = sha ? std::string(sha) : digestOf(decoded);
    bodies[std::string(installation.host) + path] = wire;
    return {path, digest, digest.substr(0, 16), true, uint32_t(wire.size())};
}

void serveZlib(const char* path, const std::vector<uint8_t>& decoded,
               const std::vector<uint8_t>& wire, const char* sha = nullptr) {
    offsets.clear();
    expectedHashes = {zlibExpected(path, decoded, wire, sha)};
    fakeGetResponse = rangedResponse;
}

bool noZlibLeftovers() {
    return !files.count(wirePartial) && !files.count(wireStage) &&
        !files.count("/weather.vnf.tmp") && liveAllocations == 0;
}

void zlib_frame_downloads_inflates_and_verifies() {
    const auto decoded = panelFrame(57000, 1);
    const auto wire = deflated(decoded);
    serveZlib("/frames/weather-z.vnz", decoded, wire);
    const std::string url = std::string(installation.host) + "/frames/weather-z.vnz";
    const auto before = frameBytesRead;
    Serial.output.clear();
    delayClosePath = "/weather.vnf.tmp";
    const auto started = std::chrono::steady_clock::now();
    assert(fetchFrame(url.c_str(), cachePath));
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
    std::cout << "host zlib transfer inflate and verification ms=" << elapsed << " bytes=" << decoded.size() << "\n";
    delayClosePath.clear();
    unsigned long streamMs=0, totalMs=0; unsigned bytes=0;
    const auto logAt = Serial.output.find("inflate ms=");
    assert(logAt != std::string::npos);
    assert(std::sscanf(Serial.output.c_str()+logAt, "inflate ms=%lu total_ms=%lu bytes=%u", &streamMs, &totalMs, &bytes)==3);
    assert(totalMs >= streamMs + 50 && bytes == decoded.size());
    assert(files.at(cachePath) == decoded && noZlibLeftovers());
    // Wire bytes are what travel, what budgets count and what the log reports.
    assert(frameBytesRead - before == wire.size() && wire.size() < 60000);
    assert(Serial.output.find("frame slot=/weather.vnf outcome=complete reason=ok status=206 got=" +
                              std::to_string(wire.size()) + " ") != std::string::npos);
    assert(Serial.output.find("total=" + std::to_string(wire.size()) + " ") != std::string::npos);
    assert(Serial.output.find("enc=zlib") != std::string::npos);
    Inkplate d;
    assert(blitFrame(cachePath, d));
    std::cout << "inflate peak heap bytes=" << (chunkSize + sizeof(tinfl_decompressor))
              << " (32768 dictionary + " << sizeof(tinfl_decompressor) << " tinfl_decompressor)"
              << " weather wire=" << wire.size() << "\n";
    fakeGetResponse = nullptr;
    expectedHashes.clear();
}

void no_inflate_log_when_output_open_fails() {
    const auto decoded = panelFrame(1000, 23);
    const auto wire = deflated(decoded);
    serveZlib("/frames/open-fail.vnz", decoded, wire);
    failOpenWrite = "/weather.vnf.tmp";
    Serial.output.clear();
    assert(!fetchFrame((std::string(installation.host) + "/frames/open-fail.vnz").c_str(), cachePath));
    assert(Serial.output.find("inflate ms=") == std::string::npos);
    failOpenWrite.clear();
}

void zlib_stored_file_verified_before_rename() {
    const auto old = files.at(cachePath);
    const auto decoded = panelFrame(1000, 22);
    const auto wire = deflated(decoded);
    serveZlib("/frames/truncated.vnz", decoded, wire);
    truncateOnClose = "/weather.vnf.tmp";
    assert(!fetchFrame((std::string(installation.host) + "/frames/truncated.vnz").c_str(), cachePath));
    assert(files.at(cachePath) == old && !files.count("/weather.vnf.tmp"));
}

void corrupt_zlib_frame_fails_closed() {
    const auto good = files.at(cachePath);
    const auto decoded = panelFrame(57000, 4);
    const auto wire = deflated(decoded);
    const auto sha = digestOf(decoded);
    const std::string url = std::string(installation.host) + "/frames/weather-z.vnz";
    const auto rejected = [&](const std::vector<uint8_t>& body, const char* reason,
                              const std::vector<uint8_t>& hashed) {
        serveZlib("/frames/weather-z.vnz", hashed, body);
        Serial.output.clear();
        assert(!fetchFrame(url.c_str(), cachePath));
        assert(files.at(cachePath) == good && noZlibLeftovers());
        assert(Serial.output.find(std::string("outcome=failed reason=") + reason) != std::string::npos);
    };
    auto flipped = wire;
    flipped[flipped.size() / 2] ^= 0x55;
    serveZlib("/frames/weather-z.vnz", decoded, flipped);
    assert(!fetchFrame(url.c_str(), cachePath) && files.at(cachePath) == good && noZlibLeftovers());
    auto longer = decoded;
    longer.resize(decoded.size() + 4096, 0x23);
    rejected(deflated(longer), "oversize", decoded);
    rejected(deflated(panelFrame(57000, 5)), "hash", decoded);
    rejected(std::vector<uint8_t>(wire.begin(), wire.end() - 10), "zlib", decoded);
    auto trailing = wire;
    trailing.insert(trailing.end(), {0, 0, 0});
    rejected(trailing, "trailing", decoded);
    auto badPixel = decoded;
    badPixel[100] = 0x66;
    rejected(deflated(badPixel), "frame", badPixel);
    std::vector<uint8_t> shorter(decoded.begin(), decoded.begin() + 500000);
    rejected(deflated(shorter), "size", shorter);
    rejected(std::vector<uint8_t>(64, 0x78), "zlib", decoded);
    // No verifiable decoded hash: nothing is downloaded at all.
    serveZlib("/frames/weather-z.vnz", decoded, wire, "");
    offsets.clear();
    assert(!fetchFrame(url.c_str(), cachePath) && offsets.empty() && files.at(cachePath) == good);
    fakeGetResponse = nullptr;
    expectedHashes.clear();
}

void resume_zlib_frame_across_wakes() {
    const auto good = files.at(cachePath);
    const auto decoded = frameWithWireSize(404744, 6);
    const auto wire = deflated(decoded);
    const char* frameUrl = "/frames/art-z.vnz";
    const std::string url = std::string(installation.host) + frameUrl;
    serveZlib(frameUrl, decoded, wire);
    ScreenInfo slot{};
    strcpy(slot.id, expectedHashes[0].id.c_str());
    strcpy(slot.url, frameUrl);
    const ManifestFrameSlots slots{nullptr, &slot};
    stopBefore = 2 * chunkSize;
    assert(!fetchFrame(url.c_str(), cachePath));
    assert(files.at(cachePath) == good && files.at(wirePartial).size() == 2 * chunkSize);
    // Next wake: new request objects, clock and manifest; the wire resumes.
    expectedHashes.clear();
    fakeClock = 0;
    lastHeaders.clear();
    serveZlib(frameUrl, decoded, wire);
    assert(!sweepFramePartials(slots));
    assert(files.at(wirePartial).size() == 2 * chunkSize);
    stopBefore = 0;
    assert(fetchFrame(url.c_str(), cachePath));
    assert(!offsets.empty() && offsets.front() == 2 * chunkSize);
    assert(files.at(cachePath) == decoded && noZlibLeftovers());
    // Power lost after the wire committed but before it was inflated.
    files[cachePath] = good;
    files[wireStage] = wire;
    assert(sweepFramePartials(slots) == (1u << 1));
    assert(files.at(cachePath) == decoded && noZlibLeftovers());
    // Recovery must verify bytes after the output file has closed too.
    files[cachePath] = good;
    files[wireStage] = wire;
    truncateOnClose = "/weather.vnf.tmp";
    assert(!sweepFramePartials(slots));
    assert(files.at(cachePath) == good && noZlibLeftovers());
    // A committed wire that does not decode to the signed frame is dropped.
    files[cachePath] = good;
    files[wireStage] = deflated(panelFrame(1000, 7));
    assert(!sweepFramePartials(slots));
    assert(files.at(cachePath) == good && noZlibLeftovers());
    fakeGetResponse = nullptr;
    expectedHashes.clear();
}

void raw_frame_from_old_server_still_works() {
    // A pre-1.1 server (or old manifest) names raw .vnf frames: no encoding.
    const auto decoded = panelFrame(3000, 8);
    bodies[std::string(installation.host) + "/frames/weather-raw.vnf"] = decoded;
    offsets.clear();
    expectedHashes = {{"/frames/weather-raw.vnf", digestOf(decoded), digestOf(decoded).substr(0, 16)}};
    fakeGetResponse = rangedResponse;
    const std::string url = std::string(installation.host) + "/frames/weather-raw.vnf";
    Serial.output.clear();
    assert(fetchFrame(url.c_str(), cachePath));
    assert(files.at(cachePath) == decoded && liveAllocations == 0);
    assert(Serial.output.find("total=960016 ") != std::string::npos);
    assert(Serial.output.find("enc=zlib") == std::string::npos);
    // Frames cached by older firmware stay loadable as they are.
    Inkplate d;
    assert(blitFrame(cachePath, d));
    fakeGetResponse = nullptr;
    expectedHashes.clear();
}

// Bench baseline: each wake moved ~560 KB in its download window, i.e.
// about 17.5 s of this 256 kbit/s + 300 ms-per-request link model. From a
// cold cache, weather (~37 KB) and a 404744-byte art frame both finish
// within two such wakes (raw, the same two frames need 1.92 MB).
void zlib_weather_and_art_complete_within_two_wakes_from_cold() {
    constexpr uint32_t wakeWindowMs = 17500;
    const auto weatherFrame = frameWithWireSize(37000, 9);
    const auto artFrame = frameWithWireSize(404744, 10);
    const auto weatherWire = deflated(weatherFrame), artWire = deflated(artFrame);
    assert(artWire.size() > 404744 - 1024 && artWire.size() < 404744 + 1024);
    const std::string weatherUrl = std::string(installation.host) + "/frames/weather-w.vnz";
    const std::string artUrl = std::string(installation.host) + "/frames/art-a.vnz";
    const auto manifest = [&] {
        expectedHashes = {zlibExpected("/frames/weather-w.vnz", weatherFrame, weatherWire),
                          zlibExpected("/frames/art-a.vnz", artFrame, artWire)};
        fakeGetResponse = rangedResponse;
    };
    files.erase("/art.vnf");
    files.erase(cachePath);
    slowLink = true;
    unsigned wakes = 0;
    for (; wakes < 2 && !(files.count(cachePath) && files.count("/art.vnf")); ++wakes) {
        fakeClock = 0;
        lastHeaders.clear();
        manifest();
        ScreenInfo art{}, weather{};
        strcpy(art.id, expectedHashes[1].id.c_str()); strcpy(art.url, "/frames/art-a.vnz");
        strcpy(weather.id, expectedHashes[0].id.c_str()); strcpy(weather.url, "/frames/weather-w.vnz");
        sweepFramePartials(ManifestFrameSlots{&art, &weather});
        // Weather first, then the remaining window goes to art.
        if (!files.count(cachePath)) fetchFrame(weatherUrl.c_str(), cachePath, wakeWindowMs);
        if (fakeClock < wakeWindowMs && !files.count("/art.vnf"))
            fetchFrame(artUrl.c_str(), "/art.vnf", wakeWindowMs - fakeClock);
        std::cout << "simulation wake=" << wakes + 1 << " ms=" << fakeClock
                  << " weather=" << files.count(cachePath) << " art=" << files.count("/art.vnf") << "\n";
    }
    assert(files.at(cachePath) == weatherFrame && files.at("/art.vnf") == artFrame);
    assert(wakes <= 2 && liveAllocations == 0);
    std::cout << "simulation weather_wire=" << weatherWire.size() << " art_wire=" << artWire.size()
              << " complete in wakes=" << wakes << "\n";
    slowLink = false;
    fakeGetResponse = nullptr;
    expectedHashes.clear();
    files.erase("/art.vnf");
}

void weather_completes_in_two_wakes_at_256_kbit() {
    prepareRanges();
    files.erase(cachePath);
    slowLink = true;
    assert(!fetchFrame(url.c_str(), cachePath, 6500));
    const size_t checkpoint = files.at(partialPath).size();
    assert(!files.count(cachePath) && checkpoint >= chunkSize && checkpoint < frame.size());

    fakeClock = 0;
    expectedHashes.clear();
    prepareRanges();
    sweepFramePartials(manifestFrames);
    assert(fetchFrame(url.c_str(), cachePath, 65000));
    assert(offsets.front() == checkpoint && fakeClock < 65000);
    assert(files.at(cachePath) == frame && !files.count(partialPath));
    slowLink = false;
    fakeGetResponse = nullptr;
    expectedHashes.clear();
}

void epaper_frame_resumes_after_sleep() {
    prepareRanges();
    auto good = frame;
    good.back() = 0x24;
    files[cachePath] = good;
    stopBefore = 2 * chunkSize;
    assert(!fetchFrame(url.c_str(), cachePath));
    assert(files.at(cachePath) == good);
    assert(files.at(partialPath).size() == stopBefore);
    const auto capacity = LittleFS.capacity;
    LittleFS.capacity = LittleFS.usedBytes() + frame.size() - stopBefore + 65536;
    assert(canStageExtraFrame(cachePath));
    LittleFS.capacity = capacity;
    // The request objects and clock are recreated on the next wake.
    expectedHashes.clear();
    fakeClock = 0;
    lastHeaders.clear();
    prepareRanges();
    sweepFramePartials(manifestFrames);
    stopBefore = 0;
    assert(fetchFrame(url.c_str(), cachePath));
    assert(!offsets.empty() && offsets.front() == 2 * chunkSize);
    assert(files.at(cachePath) == frame && !files.count(partialPath));
    fakeGetResponse = nullptr;
}

void epaper_frame_hash_checked() {
    prepareRanges();
    auto good = files.at(cachePath);
    frame.back() = 0x24; // Structurally valid, with a different SHA-256.
    assert(!fetchFrame(url.c_str(), cachePath));
    assert(!offsets.empty() && offsets.back() + chunkSize >= frame.size());
    assert(files.at(cachePath) == good && !files.count(partialPath));
    frame = good;
    fakeGetResponse = nullptr;
    expectedHashes.clear();
}

void orphaned_committed_frame_is_promoted_or_removed() {
    static FakeFS sd;
    assert(sd.mkdir("/partials"));
    setPartialSdFilesystem(sd);
    selectPartialStore(true);
    prepareRanges();
    constexpr char weatherStage[] = "/partials/frame-weather.vnf.tmp";
    constexpr char artStage[] = "/partials/frame-art.vnf.tmp";
    constexpr char otaStage[] = "/partials/ota-1.2.3";
    auto stale = frame;
    stale.back() = 0x24;
    files[cachePath] = stale;
    files[weatherStage] = frame;
    sd.files[weatherStage] = stale;
    sd.files[artStage] = frame; // Its digest belongs to weather, not art.
    sd.files[otaStage] = {1, 2, 3};
    files["/weather.vnf.tmp"] = {1};

    assert(sweepFramePartials(manifestFrames) == (1u << 1));
    assert(files.at(cachePath) == frame && !files.count(weatherStage));
    assert(!sd.files.count(weatherStage) && !sd.files.count(artStage));
    assert(!files.count("/art.vnf") && !files.count("/weather.vnf.tmp"));
    assert(sd.files.at(otaStage) == std::vector<uint8_t>({1, 2, 3}));

    files[cachePath] = stale;
    sd.files[weatherStage] = frame;
    assert(sweepFramePartials(manifestFrames) == (1u << 1));
    assert(files.at(cachePath) == frame && !sd.files.count(weatherStage));
    assert(!files.count("/weather.vnf.tmp"));
    selectPartialStore(false);
    fakeGetResponse = nullptr;
}

void epaper_sd_commit_and_fallback() {
    static FakeFS sd;
    setPartialSdFilesystem(sd, nullptr);
    selectPartialStore(true);
    prepareRanges();
    stopBefore = chunkSize;
    assert(!fetchFrame(url.c_str(), cachePath));
    assert(sd.files.at(partialPath).size() == chunkSize && !files.count(partialPath));

    expectedHashes.clear();
    sweepFramePartials(manifestFrames);
    assert(!sd.files.count(partialPath));
    prepareRanges();
    assert(!fetchFrame(url.c_str(), cachePath));
    offsets.clear();
    stopBefore = 0;
    assert(fetchFrame(url.c_str(), cachePath));
    assert(offsets.front() == chunkSize && files.at(cachePath) == frame);
    const auto old = files.at(cachePath);
    prepareRanges();
    truncateOnClose = "/weather.vnf.tmp";
    assert(!fetchFrame(url.c_str(), cachePath));
    assert(files.at(cachePath) == old && !files.count("/weather.vnf.tmp"));
    assert(!sd.files.count(partialPath) && !files.count("/weather.vnf.tmp"));

    stopBefore = chunkSize;
    assert(!fetchFrame(url.c_str(), cachePath));
    sd.offline = true;
    assert(!fetchFrame(url.c_str(), cachePath));
    assert(partialsOnFlash() && files.at(cachePath) == frame);
    offsets.clear();
    stopBefore = 0;
    assert(fetchFrame(url.c_str(), cachePath));
    assert(offsets.front() == 0 && files.at(cachePath) == frame);
}
#endif
}

int main() {
    response.assign(960016, 0x25);
    const uint8_t header[16] = {'V','N','F','1',0x40,6,0xb0,4,0,0,0,0,0,0,0,0};
    std::copy(header,header+16,response.begin());
    frame = response;
#if VENTANAL_ABORT_AFTER_BYTES > 0
    files["/weather.vnf"] = response;
    auto good = response;
    assert(!fetchFrame("http://test/frame", "/weather.vnf"));
    assert(files["/weather.vnf"] == good);
    assert(!files.count(partialPath)); // The legacy 200 has no resumable identity.
    assert(WiFi.sleeping == WIFI_PS_MAX_MODEM && liveAllocations == 0);
    assert(lastUserAgent == "Ventanal/1.1.0");
    Inkplate d;
    assert(blitFrame("/weather.vnf", d));
    assert(d.pixels[0] == 2 && d.pixels.back() == 5);
    epaper_abort_after_bytes_still_aborts();
    std::cout << "debug abort at 100000 bytes preserves boot cache\n";
#else
    assert(fetchFrame("http://test/frame", "/weather.vnf"));
    assert(readBytesCalls == 0 && largestRead <= 1460);
    assert(lastUserAgent == "Ventanal/1.1.0");
    assert(largestWrite == chunkSize && writeCalls == 60);
    assert(WiFi.sleeping == WIFI_PS_MAX_MODEM && WiFi.sleepChanges == 2 && liveAllocations == 0);
    auto good=files["/weather.vnf"];
    assert(good==response && !files.count("/weather.vnf.tmp"));
    Inkplate d;
    assert(blitFrame("/weather.vnf",d));
    assert(d.rotation==1 && d.pixels[0]==2 && d.pixels[1]==5 && d.pixels.back()==5);
    response.resize(100000); // Mid-stream disconnect must leave last good frame.
    assert(!fetchFrame("http://test/frame", "/weather.vnf"));
    assert(files["/weather.vnf"]==good && !files.count("/weather.vnf.tmp"));
    response=good; response[0]='X';
    assert(!fetchFrame("http://test/frame", "/weather.vnf"));
    assert(files["/weather.vnf"]==good);
    response=good; response.back()=0x60;
    assert(!fetchFrame("http://test/frame", "/weather.vnf"));
    assert(files["/weather.vnf"]==good);
    response=good; advertisedSize=960017;
    assert(!fetchFrame("http://test/frame", "/weather.vnf"));
    advertisedSize=960016; status=404;
    assert(!fetchFrame("http://test/frame", "/weather.vnf"));
    status=200; failRename=true;
    assert(!fetchFrame("http://test/frame", "/weather.vnf"));
    assert(files["/weather.vnf"]==good);
    assert(WiFi.sleeping == WIFI_PS_MAX_MODEM && liveAllocations == 0);
    failRename=false;
    failWrite=true;
    assert(!fetchFrame("http://test/frame", "/weather.vnf"));
    assert(files["/weather.vnf"]==good && !files.count("/weather.vnf.tmp"));
    assert(WiFi.sleeping == WIFI_PS_MAX_MODEM && liveAllocations == 0);
    failWrite=false;
    failBegin=true;
    assert(!fetchFrame("http://test/frame", "/weather.vnf"));
    assert(files["/weather.vnf"]==good && WiFi.sleeping == WIFI_PS_MAX_MODEM && liveAllocations == 0);
    failBegin=false;
    failAllocation=true;
    assert(!fetchFrame("http://test/frame", "/weather.vnf"));
    assert(files["/weather.vnf"]==good && liveAllocations == 0);
    failAllocation=false;
    WiFi.sleeping=WIFI_PS_NONE;
    assert(fetchFrame("http://test/frame", "/weather.vnf"));
    assert(WiFi.sleeping == WIFI_PS_NONE && liveAllocations == 0);
    readDelay=15000;
    const auto started=fakeClock;
    assert(!fetchFrame("http://test/frame", "/weather.vnf"));
    assert(fakeClock-started < 31000 && files["/weather.vnf"]==good);
    readDelay=0;
    files["/weather.vnf"][16]=0xf0;
    std::fill(d.pixels.begin(),d.pixels.end(),255);
    assert(!blitFrame("/weather.vnf",d));
    assert(d.pixels[0]==255); // Validate whole file before changing framebuffer.
    epaper_frame_resumes_after_sleep();
    epaper_frame_hash_checked();
    orphaned_committed_frame_is_promoted_or_removed();
    epaper_sd_commit_and_fallback();
    weather_completes_in_two_wakes_at_256_kbit();
    zlib_frame_downloads_inflates_and_verifies();
    no_inflate_log_when_output_open_fails();
    zlib_stored_file_verified_before_rename();
    corrupt_zlib_frame_fails_closed();
    resume_zlib_frame_across_wakes();
    raw_frame_from_old_server_still_works();
    zlib_weather_and_art_complete_within_two_wakes_from_cold();
    std::cout << "cache tests passed\n";
#endif
}
