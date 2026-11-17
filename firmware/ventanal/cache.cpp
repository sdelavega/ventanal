// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
#include "cache.h"
#include "logic.h"
#ifdef VENTANAL_HOST_TEST
#include "config.example.h"
#else
#include "config.h"
#endif
#include <algorithm>
#include "http.h"
#include "security.h"
#include <esp_heap_caps.h>
#include <memory>
#include <WiFi.h>
#include <LittleFS.h>

#ifndef VENTANAL_ABORT_AFTER_BYTES
#define VENTANAL_ABORT_AFTER_BYTES 0
#endif
namespace {
constexpr size_t frameSize = 960016;
constexpr size_t downloadBlockSize = 65536;
// Preserve the exact previous power-save mode on every return path.
struct DownloadPower {
    decltype(WiFi.getSleep()) previous = WiFi.getSleep();
    DownloadPower() { WiFi.setSleep(false); }
    ~DownloadPower() { WiFi.setSleep(previous); }
};
bool validate(File& f) {
    uint8_t block[512];
    if (!f || f.read(block, 16) != 16 || !validFrameHeader(block, f.size())) return false;
    size_t remaining = frameSize - 16;
    while (remaining) {
        const size_t n = std::min(remaining, sizeof(block));
        if (f.read(block, n) != n) return false;
        for (size_t i = 0; i < n; ++i)
            if ((block[i] >> 4) > 5 || (block[i] & 15) > 5) return false;
        remaining -= n;
    }
    return f.seek(16);
}
}

bool fetchFrame(const char* url, const char* path) {
    const String temporary = String(path) + ".tmp";
    // Only the uncommitted scratch file may be removed. LittleFS rename replaces
    // the destination atomically; never unlink the last good frame first.
    LittleFS.remove(temporary.c_str());
    const uint32_t started = millis();
    std::unique_ptr<uint8_t, decltype(&heap_caps_free)> block(
        static_cast<uint8_t*>(heap_caps_malloc(downloadBlockSize, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
        heap_caps_free);
    if (!block) return false;
    DownloadPower power;
    TransportClient localClient;
    auto& client=requestClient(localClient);
    HTTPClient localHttp;
    auto& http=requestHttp(localHttp);
    configureHttp(http);
    if (!beginTransport(http, client, url)) return false;
    const auto headerStart=millis();
    const int status=http.GET();
    Serial.printf("transport tls=%d handshake-headers ms=%lu status=%d\n",securityConfigured(),static_cast<unsigned long>(millis()-headerStart),status);
    if (status != 200 || http.getSize() != static_cast<int>(frameSize)) {
        finishTransport(http,false);
        return false;
    }
    const char* expected=expectedHash(url);
    if(securityConfigured() && !hex64(expected)) { finishTransport(http,false); return false; }
    FrameHash digest;
    File out = LittleFS.open(temporary.c_str(), "w");
    size_t received = 0, buffered = 0;
    uint32_t readMs = 0, writeMs = 0;
    uint32_t lastProgress = millis();
    // Use internal heap RAM so flash writes avoid PSRAM bounce copies;
    // accumulate network packets before a large filesystem write.
    // keep the Arduino loop stack free for FS/network calls.
    auto* stream = http.getStreamPtr();
    while (out && received < frameSize && millis() - started < 30000) {
        const int available = stream->available();
        if (available > 0) {
            size_t n = std::min({size_t(available), downloadBlockSize-buffered, frameSize-received});
#if VENTANAL_ABORT_AFTER_BYTES > 0
            n = std::min(n, static_cast<size_t>(VENTANAL_ABORT_AFTER_BYTES) - received);
#endif
            uint32_t phase = millis();
            const int got = stream->read(block.get()+buffered, n);
            readMs += millis() - phase;
            if (got <= 0) break;
            digest.add(block.get()+buffered,got);
            received += got;
            buffered += got;
            lastProgress = millis();
            bool flushBlock = buffered == downloadBlockSize || received == frameSize;
#if VENTANAL_ABORT_AFTER_BYTES > 0
            flushBlock = flushBlock || received >= VENTANAL_ABORT_AFTER_BYTES;
#endif
            if (flushBlock) {
                phase = millis();
                const size_t written = out.write(block.get(), buffered);
                writeMs += millis() - phase;
                if (written != buffered) break;
                buffered = 0;
            }
#if VENTANAL_ABORT_AFTER_BYTES > 0
            if (received >= VENTANAL_ABORT_AFTER_BYTES) {
                Serial.printf("download debug-abort bytes=%u cache-preserved\n", unsigned(received));
                out.flush();
                out.close();
                finishTransport(http,false);
                // Deliberately leave .tmp, like interrupted power. Next boot
                // blits the committed file before attempting any new download.
                return false;
            }
#endif
        } else {
            if (!http.connected() || millis() - lastProgress >= 15000) break;
            delay(1);
        }
    }
    if (out) { out.flush(); out.close(); }
    finishTransport(http,received==frameSize);
    const uint32_t checking = millis();
    File check = LittleFS.open(temporary.c_str(), "r");
    const bool hashOK = !securityConfigured() || digest.finish()==expected;
    if(securityConfigured()) Serial.printf("security frame-hash=%s\n",hashOK?"verified":"rejected");
    const bool valid = received == frameSize && hashOK && validate(check) && millis()-started < 30000;
    check.close();
    const bool committed = valid && LittleFS.rename(temporary.c_str(), path);
    if (!committed) LittleFS.remove(temporary.c_str());
    Serial.printf("download path=%s bytes=%u committed=%d ms=%lu\n", path, unsigned(received), committed, static_cast<unsigned long>(millis() - started));
    Serial.printf("download phases read=%lu write=%lu check=%lu ms\n", static_cast<unsigned long>(readMs), static_cast<unsigned long>(writeMs), static_cast<unsigned long>(millis() - checking));
    return committed;
}

bool blitFrame(const char* path, Inkplate& d) {
    File f = LittleFS.open(path, "r");
    if (!validate(f)) { f.close(); return false; }
    d.setRotation(VENTANAL_ROTATION);
    if (d.width() != 1600 || d.height() != 1200) { f.close(); return false; }
    uint8_t row[800];
    for (int y = 0; y < 1200; ++y) {
        if (f.read(row, sizeof(row)) != sizeof(row)) { f.close(); return false; }
        for (int x = 0; x < 1600; x += 2) {
            d.drawPixel(x, y, row[x / 2] >> 4);
            d.drawPixel(x + 1, y, row[x / 2] & 15);
        }
    }
    f.close();
    return true;
}
