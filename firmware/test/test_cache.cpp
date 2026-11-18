// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
#include "../ventanal/cache.h"
#include <LittleFS.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <esp_heap_caps.h>
#include <cassert>
#include <iostream>
int main() {
    response.assign(960016, 0x25);
    const uint8_t header[16] = {'V','N','F','1',0x40,6,0xb0,4,0,0,0,0,0,0,0,0};
    std::copy(header,header+16,response.begin());
#if VENTANAL_ABORT_AFTER_BYTES > 0
    files["/weather.vnf"] = response;
    auto good = response;
    assert(!fetchFrame("http://test/frame", "/weather.vnf"));
    assert(files["/weather.vnf"] == good);
    assert(files["/weather.vnf.tmp"].size() == 100000);
    assert(WiFi.sleeping == WIFI_PS_MAX_MODEM && liveAllocations == 0);
    assert(lastUserAgent == "Ventanal/1.0.0");
    Inkplate d;
    assert(blitFrame("/weather.vnf", d));
    assert(d.pixels[0] == 2 && d.pixels.back() == 5);
    std::cout << "debug abort at 100000 bytes preserves boot cache\n";
#else
    assert(fetchFrame("http://test/frame", "/weather.vnf"));
    assert(readBytesCalls == 0 && largestRead <= 1460);
    assert(lastUserAgent == "Ventanal/1.0.0");
    assert(largestWrite == 65536 && writeCalls == 15);
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
    std::cout << "cache tests passed\n";
#endif
}
