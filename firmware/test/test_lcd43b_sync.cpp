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
#include <string>
#include <vector>
#include <ctime>

namespace {

struct CountingWatchdog : lcd::Watchdog {
    unsigned feeds = 0;
    void feed() override { ++feeds; }
};

const uint8_t* protectedSpare;
std::vector<std::string> cacheEvents;

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
    readBytesCalls = 0;
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
    cacheEvents.clear();
    fakeTz.clear();
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
        advertisedSize = int(lcd::FRAME_BYTES);
        delayedFrom = frame.size();              // ...arrives after 300 ms
        delayedUntilMs = 300;
        CountingWatchdog watchdog;
        std::vector<uint8_t> buffer(lcd::FRAME_BYTES + 4096, 0);   // guard band
        assert(!lcd::downloadFrame("http://host", art.info, "art",
                                   buffer.data(), watchdog));
        assert(strstr(Serial.output.c_str(), "excess=1"));
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
        advertisedSize = int(lcd::FRAME_BYTES);
        holdConnection = true;                 // no hang-up, no extra bytes
        CountingWatchdog watchdog;
        std::vector<uint8_t> buffer(lcd::FRAME_BYTES, 0);
        assert(!lcd::downloadFrame("http://host", art.info, "art",
                                   buffer.data(), watchdog));
        assert(!strstr(Serial.output.c_str(), "excess=1"));
        assert(files["/art.vnr"] == previous);  // previous cache preserved
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
        files["/solar.vnr"] = std::vector<uint8_t>(64, 0x5A);   // stale cache
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
        // The worker erased the dropped cache file (never in loop()).
        assert(!files.count("/solar.vnr"));
        assert(strstr(Serial.output.c_str(), "cache removed slot=solar"));
        assert(!strcmp(result.frameIds[0], art.info.id));
        assert(!strcmp(result.frameIds[1], weather.info.id));
        assert(!lcd::anyRequiredFrameFailed(result));   // nothing required failed

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
        assert(!lcd::anyRequiredFrameFailed(again));   // nothing was required
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
        lcd::runRefresh("http://host","lcd43b",ids,result,watchdog,settings);
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
        lcd::runRefresh("http://host","lcd43b",ids,result,watchdog,settings);
        assert(!result.frameOk[0] && result.frameOk[1] && !result.frameOk[3]);
        assert(requestedUrls.size()==2);
    }
    {   // An override selected from RAM survives replacing its spare on disk.
        resetFakes();
        const auto oldFrame = solidFrame(0xF800);
        const auto newFrame = solidFrame(0x07E0);
        const Entry art = makeEntry("art", oldFrame);
        const Entry weather = makeEntry("weather", newFrame);
        const Entry spare = makeEntry("spare0", newFrame);
        assert(lcd::cachedFrameValid(oldFrame.data(), art.info.id));
        assert(!lcd::cachedFrameValid(newFrame.data(), art.info.id));
        responseBody = manifestBody(art, weather);
        responseBody.insert(responseBody.size() - 1, ",\"spares\":[" + spare.body + "]");
        urlResponses["http://host" + spare.url] = newFrame;
        sizeFromResponse = true;
        files["/spare0.vnr"] = oldFrame;
        char ids[lcd::SCREEN_SLOTS][17]{};
        strcpy(ids[0], art.info.id);
        strcpy(ids[1], weather.info.id);
        strcpy(ids[3], art.info.id);
        std::vector<uint8_t> staging(lcd::FRAME_BYTES);
        protectedSpare = oldFrame.data();
        lcd::ResidentCache cache;
        cache.staging = staging.data();
        cache.allocated[3] = true;
        cache.protectSpare = [](int slot) {
            assert(slot == 3);
            cacheEvents.emplace_back("protect");
            return protectedSpare;
        };
        cache.flashDone = [](const char* operation) { cacheEvents.emplace_back(operation); };
        cache.publish = [](int slot, const char* id, const uint8_t* bytes) {
            assert(slot == 3);
            cacheEvents.emplace_back(id ? "publish" : "failed");
            if (id) {
                assert(lcd::cachedFrameValid(bytes, id));
                assert(files.at("/spare0.vnr") == std::vector<uint8_t>(bytes, bytes + lcd::FRAME_BYTES));
            }
        };
        CountingWatchdog watchdog;
        lcd::RefreshResult result;
        failRename = true;
        lcd::runRefresh("http://host", "lcd43b", ids, result, watchdog, {}, "", &cache);
        assert(result.frameFailed[3] && files.at("/spare0.vnr") == oldFrame);
        assert(!files.count("/override.vnr.tmp"));
        assert((cacheEvents == std::vector<std::string>{"protect", "frame-commit", "failed"}));

        cacheEvents.clear();
        failRename = false;
        lcd::runRefresh("http://host", "lcd43b", ids, result, watchdog, {}, "", &cache);
        assert(result.frameOk[3] && !result.frameFailed[3]);
        assert(files.at("/override.vnr") == oldFrame && files.at("/spare0.vnr") == newFrame);
        assert((cacheEvents == std::vector<std::string>{"protect", "frame-commit", "frame-commit", "publish"}));
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
        assert(result.frameFailed[0] && result.frameFailed[1]);   // required
        assert(lcd::anyRequiredFrameFailed(result));   // the pass failed
        assert(!files.count("/art.vnr")); // nothing partial committed
        const uint32_t elapsed = fakeClock - started;
        assert(elapsed < 2 * lcd::TRANSFER_BUDGET_MS + 80000);   // bounded
        assert(watchdog.feeds > 5000);   // fed during both attempts on each frame
        assert(std::count(requestedUrls.begin(), requestedUrls.end(), "http://host" + weather.url) == 2);
        assert(strstr(Serial.output.c_str(), "outcome=body-idle"));
        assert(strstr(Serial.output.c_str(), "download slot=weather retry=body"));
    }
    {   // Connection-close framing must actually ask for closure: the
        // outgoing request carries Connection: close (keep-alive disabled),
        // so a persistent-capable HTTP/1.1 server — one that keeps the
        // socket open after the body under keep-alive — hangs up and the
        // frame commits (round-4 Important 1: the default keep-alive made
        // every such download time out on the close grace).
        resetFakes();
        const std::vector<uint8_t> frame = solidFrame(0x001F);
        Entry art = makeEntry("art", frame);
        response = frame;
        advertisedSize = int(lcd::FRAME_BYTES);
        persistentServer = true;      // stays open only under keep-alive
        CountingWatchdog watchdog;
        std::vector<uint8_t> buffer(lcd::FRAME_BYTES, 0);
        assert(lcd::downloadFrame("http://host", art.info, "art",
                                  buffer.data(), watchdog));
        assert(lastConnectionHeader == "close");   // asked the server to close
        assert(!reuse);                            // keep-alive disabled
        assert(files.count("/art.vnr"));           // committed
    }
    {   // Manifest ok but a required frame fails: the failure is tracked per
        // slot, separately from manifest success (round-4 Important 2 used
        // to show UP TO DATE over stale artwork). Partial success across
        // slots: art commits while weather's answer never delivers a body.
        resetFakes();
        const std::vector<uint8_t> artFrame = solidFrame(0xF800);
        const std::vector<uint8_t> weatherFrame = solidFrame(0x07E0);
        Entry art = makeEntry("art", artFrame);
        Entry weather = makeEntry("weather", weatherFrame);
        responseBody = manifestBody(art, weather);
        urlResponses["http://host/devices/lcd43b/manifest.json"] =
            std::vector<uint8_t>(responseBody.begin(), responseBody.end());
        urlResponses["http://host" + art.url] = artFrame;
        // Weather's server answer truncates: no full body ever arrives.
        urlResponses["http://host" + weather.url] =
            std::vector<uint8_t>(1024, 0);
        sizeFromResponse = true;
        char currentIds[lcd::SCREEN_SLOTS][17] = {};
        lcd::RefreshResult result;
        CountingWatchdog watchdog;
        lcd::runRefresh("http://host", "lcd43b", currentIds, result, watchdog);
        assert(result.manifestOk);                 // the manifest itself arrived
        assert(result.frameOk[0] && !result.frameOk[1]);
        assert(!result.frameFailed[0]);            // art: required and done
        assert(result.frameFailed[1]);            // weather: required, failed
        assert(!result.frameFailed[2] && result.dropSlot[2]);   // drop != fail
        assert(lcd::anyRequiredFrameFailed(result));   // pass is a failure
        assert(files.count("/art.vnr"));          // the partial success kept
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
        urlResponses["http://host/devices/lcd43b/manifest.json"] =
            std::vector<uint8_t>(responseBody.begin(), responseBody.end());
        urlResponses["http://host" + art.url] = artFrame;
        urlResponses["http://host" + weather.url] = weatherFrame;
        sizeFromResponse = true;
        failAllocation = true;                     // no staging buffer at all
        char currentIds[lcd::SCREEN_SLOTS][17] = {};
        lcd::RefreshResult result;
        CountingWatchdog watchdog;
        lcd::runRefresh("http://host", "lcd43b", currentIds, result, watchdog);
        assert(result.manifestOk);
        assert(result.frameFailed[0] && result.frameFailed[1]);   // required
        assert(lcd::anyRequiredFrameFailed(result));
        assert(!files.count("/art.vnr"));          // nothing committed
    }
    {   // The shared queue wire format is sent on the device manifest GET.
        resetFakes();
        const auto frame=solidFrame(0xffff);
        const auto art=makeEntry("art",frame), weather=makeEntry("weather",frame);
        responseBody=manifestBody(art,weather);
        sizeFromResponse=true;
        char ids[lcd::SCREEN_SLOTS][17]={};
        lcd::RefreshResult result; CountingWatchdog watchdog;
        lcd::runRefresh("http://host","lcd43b",ids,result,watchdog,lcd::DeviceSettings{},
                        "aaaaaaaaaaaaaaaa~bbbbbbbbbbbbbbbb");
        assert(result.manifestOk);
        assert(requestedUrls[0]=="http://host/devices/lcd43b/manifest.json?skips=aaaaaaaaaaaaaaaa~bbbbbbbbbbbbbbbb");
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

        // A dropped slot clears its id here; the worker already erased the
        // cache file (flash erases must never run in loop()).
        strcpy(state.cachedIds[2], "cccccccccccccccc");
        lcd::RefreshResult dropped;
        dropped.manifestOk = true;
        dropped.manifest = published;
        dropped.dropSlot[2] = true;
        lcd::applyRefresh(state, screenIndex, spareCursor, prefs, dropped,
                          false);
        assert(!state.cachedIds[2][0]);
        char ids[lcd::SCREEN_SLOTS][17] = {};
        lcd::loadCacheIds(prefs, ids);
        assert(!ids[2][0]);            // the NVS entry cleared too

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
    {   // Offline reboot with retained RTC time: the timezone is
        // initialised in setup (connectivity-independent), so the persisted
        // LOCAL quiet window evaluates in the configured zone, not UTC.
        resetFakes();
        Preferences prefs;
        assert(prefs.begin("ventanal", false));
        lcd::DeviceManifest published;         // what the last online sync stored
        published.quietStart = 1380;            // 23:00 local
        published.quietEnd = 360;               // 06:00 local
        lcd::saveQuiet(prefs, published);
        lcd::DeviceManifest rebooted;           // fresh boot: NVS only, no network
        assert(lcd::loadQuiet(prefs, rebooted));
        // setup() initialises the configured timezone before any refresh.
        lcd::configureTimezone();
        assert(fakeTz == VENTANAL_TZ);
        // Retained RTC: 2026-11-17 06:30 UTC, synced before the reboot.
        std::tm utc{};
        utc.tm_year = 2026 - 1900;
        utc.tm_mon = 10;
        utc.tm_mday = 17;
        utc.tm_hour = 6;
        utc.tm_min = 30;
        const time_t instant = timegm(&utc);
        setenv("TZ", fakeTz.c_str(), 1);       // MST (UTC-7) in November
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
