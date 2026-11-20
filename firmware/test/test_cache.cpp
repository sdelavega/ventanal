// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
#include "../ventanal/cache.h"
#include "../ventanal/security.h"
#include "../common/resume/storage.h"
#include <LittleFS.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <esp_heap_caps.h>
#include <cassert>
#include <iostream>

namespace {
const std::string url = std::string(installation.host) + "/frame";
constexpr char cachePath[] = "/weather.vnf";
constexpr char partialPath[] = "/partials/frame-weather.vnf.tmp.part";
constexpr size_t chunkSize = 32768;
std::vector<uint8_t> frame;
std::vector<size_t> offsets;
size_t stopBefore = 0;

int rangedResponse() {
    const auto range = lastHeaders.find("Range");
    if (range == lastHeaders.end()) return 503;
    unsigned start, end;
    assert(sscanf(range->second.c_str(), "bytes=%u-%u", &start, &end) == 2);
    offsets.push_back(start);
    if (stopBefore && start >= stopBefore) {
        delay(30000);
        return HTTPC_ERROR_READ_TIMEOUT;
    }
    assert(end < frame.size() && end - start + 1 <= chunkSize);
    response.assign(frame.begin()+start, frame.begin()+end+1);
    advertisedSize = response.size();
    fakeResponseHeaders["Content-Range"] = "bytes " + std::to_string(start) + "-" +
        std::to_string(end) + "/" + std::to_string(frame.size());
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
    sweepFramePartials();
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

void epaper_sd_commit_and_fallback() {
    static FakeFS sd;
    setPartialSdFilesystem(sd, nullptr);
    selectPartialStore(true);
    prepareRanges();
    stopBefore = chunkSize;
    assert(!fetchFrame(url.c_str(), cachePath));
    assert(sd.files.at(partialPath).size() == chunkSize && !files.count(partialPath));

    expectedHashes.clear();
    sweepFramePartials();
    assert(!sd.files.count(partialPath));
    prepareRanges();
    assert(!fetchFrame(url.c_str(), cachePath));
    offsets.clear();
    stopBefore = 0;
    assert(fetchFrame(url.c_str(), cachePath));
    assert(offsets.front() == chunkSize && files.at(cachePath) == frame);
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
    assert(files.at(partialPath).size() == 3 * chunkSize);
    assert(WiFi.sleeping == WIFI_PS_MAX_MODEM && liveAllocations == 0);
    assert(lastUserAgent == "Ventanal/1.0.0");
    Inkplate d;
    assert(blitFrame("/weather.vnf", d));
    assert(d.pixels[0] == 2 && d.pixels.back() == 5);
    epaper_abort_after_bytes_still_aborts();
    std::cout << "debug abort at 100000 bytes preserves boot cache\n";
#else
    assert(fetchFrame("http://test/frame", "/weather.vnf"));
    assert(readBytesCalls == 0 && largestRead <= 1460);
    assert(lastUserAgent == "Ventanal/1.0.0");
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
    epaper_sd_commit_and_fallback();
    std::cout << "cache tests passed\n";
#endif
}
