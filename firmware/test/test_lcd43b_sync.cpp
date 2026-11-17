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

#include "Arduino.h"
#include "esp_sntp.h"
#include "HTTPClient.h"
#include "LittleFS.h"
#include "Preferences.h"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

struct CountingWatchdog : lcd::Watchdog {
    unsigned feeds = 0;
    void feed() override { ++feeds; }
};

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

Entry makeEntry(const std::string& name, const std::vector<uint8_t>& frame) {
    Entry entry;
    const std::string sha = sha256Hex(frame);
    strlcpy(entry.info.id, sha.substr(0, 16).c_str(), sizeof entry.info.id);
    strlcpy(entry.info.sha256, sha.c_str(), sizeof entry.info.sha256);
    entry.url = "/devices/lcd43b/frames/" + name + "-"
              + std::string(entry.info.id) + ".vnr";
    strlcpy(entry.info.url, entry.url.c_str(), sizeof entry.info.url);
    entry.info.present = true;
    entry.body = "{\"id\":\"" + std::string(entry.info.id) + "\",\"url\":\""
               + entry.url + "\",\"size\":" + std::to_string(frame.size())
               + ",\"sha256\":\"" + sha + "\"}";
    return entry;
}

std::string manifestBody(const Entry& art, const Entry& weather) {
    return "{\"version\":1,\"screens\":{\"art\":" + art.body
         + ",\"weather\":" + weather.body
         + "},\"quiet\":{\"start\":\"23:00\",\"end\":\"06:00\"}}";
}

void resetFakes() {
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
    readBytesCalls = 0;
    fakeClock = 0;
    fakeSyncStatus = SNTP_SYNC_STATUS_COMPLETED;
}

}  // namespace

int main() {
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
        assert(!lcd::downloadFrame("http://host", art.info, "art",
                                   buffer.data(), watchdog));
        assert(strstr(Serial.output.c_str(), "excess=1"));
        assert(largestRead <= lcd::CHUNK_BYTES);   // chunk cap held
        for (size_t i = lcd::FRAME_BYTES; i < buffer.size(); ++i)
            assert(buffer[i] == 0);                // guard band never written
    }
    {   // Mid-stream disconnect: the download fails without committing a
        // partial frame to the cache.
        resetFakes();
        const std::vector<uint8_t> frame = solidFrame(0x07E0);
        Entry art = makeEntry("art", frame);
        response.assign(frame.begin(), frame.begin() + 1024);   // then hangs up
        advertisedSize = int(lcd::FRAME_BYTES);
        CountingWatchdog watchdog;
        std::vector<uint8_t> buffer(lcd::FRAME_BYTES, 0);
        assert(!lcd::downloadFrame("http://host", art.info, "art",
                                   buffer.data(), watchdog));
        assert(!files.count("/art.vnr"));
    }
    {   // A valid frame downloads, verifies and commits via .tmp+rename.
        resetFakes();
        const std::vector<uint8_t> frame = solidFrame(0x001F);
        Entry art = makeEntry("art", frame);
        response = frame;
        advertisedSize = int(lcd::FRAME_BYTES);
        CountingWatchdog watchdog;
        std::vector<uint8_t> buffer(lcd::FRAME_BYTES, 0);
        assert(lcd::downloadFrame("http://host", art.info, "art",
                                  buffer.data(), watchdog));
        assert(files.count("/art.vnr"));
        assert(files["/art.vnr"].size() == lcd::FRAME_BYTES);
        assert(!files.count("/art.vnr.tmp"));
        assert(!strstr(Serial.output.c_str(), "excess=1"));
    }
    {   // runRefresh end to end: manifest and frames arrive; solar absent
        // from the manifest stages a drop; a second pass skips downloads.
        resetFakes();
        const std::vector<uint8_t> artFrame = solidFrame(0xF800);
        const std::vector<uint8_t> weatherFrame = solidFrame(0x07E0);
        Entry art = makeEntry("art", artFrame);
        Entry weather = makeEntry("weather", weatherFrame);
        responseBody = manifestBody(art, weather);
        urlResponses["http://host/devices/lcd43b/manifest.json"] =
            std::vector<uint8_t>(responseBody.begin(), responseBody.end());
        urlResponses["http://host" + art.url] = artFrame;
        urlResponses["http://host" + weather.url] = weatherFrame;
        sizeFromResponse = true;
        char currentIds[lcd::SCREEN_SLOTS][17] = {};
        lcd::RefreshResult result;
        CountingWatchdog watchdog;
        lcd::runRefresh("http://host", "lcd43b", currentIds, result, watchdog);
        assert(result.manifestOk);
        assert(result.frameOk[0] && result.frameOk[1]);
        assert(!result.frameOk[2]);
        assert(result.dropSlot[2]);       // solar vanished from the manifest
        for (int slot = 3; slot < lcd::SCREEN_SLOTS; ++slot)
            assert(result.dropSlot[slot]);   // unpublished spare slots
        assert(files.count("/art.vnr") && files.count("/weather.vnr"));
        assert(!strcmp(result.frameIds[0], art.info.id));
        assert(!strcmp(result.frameIds[1], weather.info.id));

        // Second pass with the committed ids: both slots are skipped and
        // stage their current ids (no new downloads at all).
        for (int slot = 0; slot < lcd::SCREEN_SLOTS; ++slot)
            strlcpy(currentIds[slot], result.frameIds[slot], 17);
        readBytesCalls = 0;
        lcd::RefreshResult again;
        lcd::runRefresh("http://host", "lcd43b", currentIds, again, watchdog);
        assert(again.manifestOk);
        assert(again.frameOk[0] && again.frameOk[1]);
        assert(!strcmp(again.frameIds[0], currentIds[0]));
        assert(!strcmp(again.frameIds[1], currentIds[1]));
        assert(readBytesCalls == 0);      // nothing re-downloaded
        assert(strstr(Serial.output.c_str(), "skipped=unchanged"));
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
        urlResponses["http://host/devices/lcd43b/manifest.json"] =
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
        lcd::runRefresh("http://host", "lcd43b", currentIds, result, watchdog);
        assert(result.manifestOk);        // the manifest itself arrived
        assert(!result.frameOk[0] && !result.frameOk[1]);
        assert(!result.dropSlot[0] && !result.dropSlot[1]);
        assert(!files.count("/art.vnr")); // nothing partial committed
        const uint32_t elapsed = fakeClock - started;
        assert(elapsed < 2 * lcd::TRANSFER_BUDGET_MS + 80000);   // bounded
        assert(watchdog.feeds > 30000);   // fed during both stalls
    }
    {   // Manifest failure: nothing staged, nothing downloaded.
        resetFakes();
        status = 500;
        advertisedSize = 0;
        char currentIds[lcd::SCREEN_SLOTS][17] = {};
        lcd::RefreshResult result;
        CountingWatchdog watchdog;
        lcd::runRefresh("http://host", "lcd43b", currentIds, result, watchdog);
        assert(!result.manifestOk);
        for (int slot = 0; slot < lcd::SCREEN_SLOTS; ++slot)
            assert(!result.frameOk[slot] && !result.dropSlot[slot]);
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
        assert(lcd::applyRefresh(state, screenIndex, spareCursor, prefs,
                                 result, false));
        assert(!state.offline);
        assert(!strcmp(state.cachedIds[0], "aaaaaaaaaaaaaaaa"));
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

        // A dropped slot removes the cache file and clears its id.
        files["/solar.vnr"] = {1};
        strcpy(state.cachedIds[2], "cccccccccccccccc");
        lcd::RefreshResult dropped;
        dropped.manifestOk = true;
        dropped.manifest = published;
        dropped.dropSlot[2] = true;
        lcd::applyRefresh(state, screenIndex, spareCursor, prefs, dropped,
                          false);
        assert(!files.count("/solar.vnr"));
        assert(!state.cachedIds[2][0]);
        assert(strstr(Serial.output.c_str(), "cache removed slot=solar"));

        // A shrunk screen list normalises the selection and persists it.
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
        assert(lcd::loadSelection(prefs, stored, cursor));
        assert(stored == 0);
    }
    {   // redrawNeeded truth table (host-pinned): new content, first draw,
        // offline change or an explicit request each force a redraw.
        assert(lcd::redrawNeeded(false, false, false, false));   // first draw
        assert(lcd::redrawNeeded(false, false, true, true));    // offline flip
        assert(lcd::redrawNeeded(false, true, true, false));    // new content
        assert(lcd::redrawNeeded(true, false, true, false));    // requested
        assert(!lcd::redrawNeeded(false, false, true, false));  // nothing new
    }
    puts("lcd43b sync: bounded reads, excess rejection, stall budgets,"
         " watchdog feeds, staged results, redraw rules passed");
    return 0;
}
