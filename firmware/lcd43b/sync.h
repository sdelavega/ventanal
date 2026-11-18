// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
#pragma once

// The lcd43b refresh engine, host-tested (test_lcd43b_sync). A refresh
// pass runs on a short-lived FreeRTOS worker task so loop() keeps polling
// touch and servicing the backlight while Wi-Fi waits, NTP waits and up to
// six 60 s frame transfers run (Important fix: refresh used to block the
// loop for the whole pass). The worker receives and decodes frames into
// scratch; the main loop owns the live slots and all cache writes.

#include <LittleFS.h>
#include <Preferences.h>

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "frame.h"
#include "deferred_save.h"
#include "log.h"
#include "manifest.h"
#include "persist.h"
#include "screens.h"

namespace lcd {

// Slot names double as the LittleFS cache file names (art, weather, solar,
// spare0..2).
inline const char* const SLOT_NAMES[SCREEN_SLOTS] = {
    "art", "weather", "solar", "spare0", "spare1", "spare2"};

struct ResidentCache {
    uint8_t* staging = nullptr; // decoded scratch, reused after each hand-off
    bool allocated[SCREEN_SLOTS] = {};
    // The synchronous hand-off ends before staging may be reused. An empty
    // info pointer means drop; otherwise body contains the verified wire bytes.
    bool (*publish)(int slot, const FrameInfo* info, const FrameBody& body) = nullptr;
    bool forceDownloads = false;
    // Optional loop-task hand-off when a wire allocation runs out of PSRAM.
    void (*reclaimWireMemory)() = nullptr;
};

// Budgets (ms). The transfer budget bounds each stalled frame download;
// the watchdog timeout must exceed the longest blocking call (one HTTP
// GET with its connect + header timeouts).
constexpr uint32_t WIFI_TIMEOUT_MS = 15000;
constexpr uint32_t NTP_TIMEOUT_MS = 10000;
constexpr uint32_t HTTP_TIMEOUT_MS = 10000;
constexpr uint32_t TRANSFER_BUDGET_MS = 60000;
constexpr uint32_t BODY_IDLE_MS = 6000;
// Bounded completion check for the connection-close framing below: how long
// a fully received frame waits for the server to hang up before the body
// is treated as incomplete (fail closed).
constexpr uint32_t FRAME_CLOSE_GRACE_MS = 1000;
constexpr size_t CHUNK_BYTES = 32768;
constexpr uint32_t WATCHDOG_TIMEOUT_MS = 30000;

// Watchdog hooks the refresh worker feeds while it runs; the sketch wires
// this to the ESP-IDF task watchdog (subscribed per worker run), host
// tests to observers. Feeding happens on every progress step, so a
// wedged worker stops feeding and the task watchdog resets the board.
class Watchdog {
public:
    virtual ~Watchdog() = default;
    virtual void feed() = 0;
};

// What one refresh pass found. Everything here is staged by the worker
// and applied by the main loop.
struct RefreshResult {
    bool wifiOk = false;
    bool timeOk = false;
    bool manifestOk = false;
    DeviceManifest manifest{};
    // Per cache slot: a verified frame was published (with its new id), or
    // the screen vanished from the manifest (drop the cached frame). A
    // failed download stages neither — the previous cache entry stays —
    // but a slot whose required download failed (timeout, verification
    // or no staging buffer) sets frameFailed, so a manifest-only
    // success can never report as a fully current pass (the sketch shows
    // DOWNLOAD FAILED instead of UP TO DATE over stale artwork).
    bool frameOk[SCREEN_SLOTS] = {};
    char frameIds[SCREEN_SLOTS][17] = {};
    bool dropSlot[SCREEN_SLOTS] = {};
    bool frameFailed[SCREEN_SLOTS] = {};
};

// True when the manifest arrived but at least one frame it required this
// pass failed: the pass is a failure for feedback purposes even though
// manifestOk is set (partial success across slots included).
inline bool anyRequiredFrameFailed(const RefreshResult& result) {
    if (!result.manifestOk) return false;
    for (int slot = 0; slot < SCREEN_SLOTS; ++slot)
        if (result.frameFailed[slot]) return true;
    return false;
}

// Live sketch state that applyRefresh manages. `offline` reflects the
// last applied refresh (none has failed at boot), so a boot blit shows no
// OFFLINE mark until a refresh actually fails.
struct RefreshState {
    DeviceManifest manifest{};
    char cachedIds[SCREEN_SLOTS][17] = {};
    bool offline = false;
    bool drawn = false;   // set by the caller after a successful blit
};

// Redraw decision after a refresh pass (pinned by test_lcd43b_sync): the
// screen must be re-blitted when asked, when new content arrived, while
// nothing valid has ever been drawn, and whenever the OFFLINE mark would
// change (a failed scheduled refresh after a successful display used to
// leave the old frame without its offline indication).
inline bool redrawNeeded(bool requested, bool newContent, bool drawn,
                         bool offlineChanged) {
    return requested || newContent || !drawn || offlineChanged;
}

// Initialise the configured timezone (configTzTime) so local time — and
// therefore the persisted quiet window — is evaluated in the configured
// zone from boot, independently of connectivity. setup() calls this
// before the first refresh: an offline reboot with retained RTC time used
// to evaluate the persisted LOCAL quiet hours in UTC, keeping the panel
// lit through the night (pinned by test_lcd43b_sync).
void configureTimezone();

// Receive and verify exactly the advertised wire bytes. The caller owns a
// buffer at least info.size bytes long. Check connection close so even a
// delayed packet beyond the advertised size rejects the response.
bool receiveFrame(const char* hostUrl, const FrameInfo& info,
                  const char* slotName, uint8_t* buffer, Watchdog& watchdog);

bool writeCachedFrame(const char* path, const FrameBody& body);
bool copyCachedFrame(const char* path, const char* source);
String residentPath(int slot, bool alternate);
bool persistCacheSlot(Preferences& prefs, int slot, CacheLocation& current,
                      const DeferredSave& save);
void sweepCacheTemps();

// One full refresh pass: Wi-Fi, NTP, the device manifest, then every
// changed frame (unchanged slots stage their current id; absent screens
// stage a drop). Blocking by design — run it on the worker task.
void runRefresh(const char* hostUrl, const char* deviceId,
                const char currentIds[SCREEN_SLOTS][17],
                RefreshResult& result, Watchdog& watchdog,
                const DeviceSettings& settings, const char* skips, const ResidentCache& cache);

// Apply a finished pass to the live state (single writer: the main loop).
// Persists the validated quiet window and publish shape, normalises the
// selection to the (possibly shrunk) screen list, and returns whether the
// screen must be re-blitted. Individual frames were already published.
inline bool applyRefresh(RefreshState& state, int& screenIndex,
                        int& spareCursor, Preferences& prefs,
                        const RefreshResult& result, bool requested,
                        const DeviceSettings& settings = DeviceSettings{},
                        bool* flashTouched = nullptr) {
    const bool wasOffline = state.offline;
    const int previousSlot=screenSlot(screenIndex,state.manifest.screens[2].present,
                                      state.manifest.spareCount,settings);
    if (result.manifestOk) {
        state.manifest = result.manifest;
        const bool quietChanged = saveQuiet(prefs, state.manifest);
        const bool shapeChanged = savePublishShape(prefs, state.manifest);
        if (flashTouched) *flashTouched = quietChanged || shapeChanged;
        remapSelection(screenIndex, spareCursor, previousSlot, state.manifest.screens[2].present,
                          state.manifest.spareCount,settings);
    }
    state.offline = !result.manifestOk;
    return redrawNeeded(requested, result.manifestOk, state.drawn,
                        state.offline != wasOffline);
}

}  // namespace lcd
