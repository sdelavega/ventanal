// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
#pragma once

// Persisted cross-reboot state for the lcd43b sketch, kept in NVS under the
// phase-1 "ventanal" Preferences namespace:
//   - the validated quiet window (the manifest's quiet block, so an
//     offline reboot still honours quiet hours — pinned by
//     test_lcd43b_persist);
//   - the clock format (default 24 h), retained across offline reboots;
//   - the published screen shape (solar present, spare count), so the
//     boot-time screen list matches the cache before the first manifest
//     fetch and the selection can be normalised;
//   - the cached frame id per slot and the selection (phase-1 keys).
// All writes happen from the main loop only (the refresh worker stages
// results; applyRefresh commits), so NVS is single-writer.

#include <Preferences.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "manifest.h"
#include "screens.h"
#include "board.h"
#include "settings.h"

namespace lcd {

// Settings use their own namespace and a dedicated Preferences handle (never
// pass the live ventanal handle). Explicit wire bytes avoid struct padding/ABI
// drift; checksum rejects corruption, version/length reject incompatible data.
inline uint32_t settingsChecksum(const uint8_t* b) {
    uint32_t h=2166136261u;
    for(int i=0;i<8;++i) h=(h^b[i])*16777619u;
    return h;
}
inline bool saveSettings(Preferences& prefs, const DeviceSettings& s) {
    if(!validSettings(s) || !prefs.begin("lcdset",false)) return false;
    uint8_t b[12]={1,s.brightness,s.timeoutMin,
        uint8_t(s.showArt | (s.showWeather<<1) | (s.showSolar<<2)),
        uint8_t(s.batteryFullMv),uint8_t(s.batteryFullMv>>8),0,0};
    const uint32_t hash=settingsChecksum(b);
    for(int i=0;i<4;++i) b[8+i]=uint8_t(hash>>(8*i));
    const bool saved = prefs.putBytes("settings", b, sizeof b) == sizeof b;
    prefs.end();
    return saved;
}
inline bool loadSettings(Preferences& prefs, DeviceSettings& s) {
    s=defaultSettings(POWER_SOURCE_DETECTABLE);
    if(!prefs.begin("lcdset",true)) return false;
    uint8_t b[12]={};
    const bool read=prefs.getBytesLength("settings")==sizeof b
        && prefs.getBytes("settings",b,sizeof b)==sizeof b;
    prefs.end();
    uint32_t hash=0;
    for(int i=0;i<4;++i) hash|=uint32_t(b[8+i])<<(8*i);
    if(!read || b[0]!=1 || b[3]>7 || b[6] || b[7] || hash!=settingsChecksum(b)) return false;
    DeviceSettings candidate;
    candidate.brightness=b[1]; candidate.timeoutMin=b[2];
    candidate.showArt=b[3]&1; candidate.showWeather=b[3]&2; candidate.showSolar=b[3]&4;
    candidate.batteryFullMv=int(b[4])|(int(b[5])<<8);
    if(!validSettings(candidate)) return false;
    s=candidate; return true;
}

// "no quiet window stored" marker for the u16 keys.
constexpr uint16_t QUIET_NONE = 0xFFFF;

// Persist the (manifest-validated) quiet window; an invalid or absent
// window clears any previously stored one.
inline void saveQuiet(Preferences& prefs, const DeviceManifest& manifest) {
    const uint16_t start = manifest.quietValid() ? uint16_t(manifest.quietStart) : QUIET_NONE;
    const uint16_t end = manifest.quietValid() ? uint16_t(manifest.quietEnd) : QUIET_NONE;
    if (prefs.getUShort("quiet-start", QUIET_NONE) != start) prefs.putUShort("quiet-start", start);
    if (prefs.getUShort("quiet-end", QUIET_NONE) != end) prefs.putUShort("quiet-end", end);
}

// Load the persisted quiet window into the (freshly booted) manifest.
// Returns false when nothing valid was stored.
inline bool loadQuiet(Preferences& prefs, DeviceManifest& manifest) {
    const uint16_t start = prefs.getUShort("quiet-start", QUIET_NONE);
    const uint16_t end = prefs.getUShort("quiet-end", QUIET_NONE);
    if (start == QUIET_NONE || end == QUIET_NONE) return false;
    manifest.quietStart = start;
    manifest.quietEnd = end;
    return manifest.quietValid();
}

// Persist the published screen shape so a boot before any manifest fetch
// can still normalise the selection against the right list. Keep the
// manifest clock preference alongside it for the same offline boot path.
inline void savePublishShape(Preferences& prefs, const DeviceManifest& m) {
    if (prefs.getUChar("solar-pub", 0) != m.screens[2].present)
        prefs.putUChar("solar-pub", m.screens[2].present);
    if (prefs.getUChar("spare-count", 0) != m.spareCount)
        prefs.putUChar("spare-count", m.spareCount);
    if (prefs.getUChar("clock24h", 1) != m.clock24h)
        prefs.putUChar("clock24h", m.clock24h);
}

inline void loadPublishShape(Preferences& prefs, DeviceManifest& m) {
    m.clock24h = prefs.getUChar("clock24h", 1) != 0;
    m.screens[2].present = prefs.getUChar("solar-pub", 0) != 0;
    m.spareCount = uint8_t(prefs.getUChar("spare-count", 0) % (SPARE_SLOTS + 1));
}

// Cache ids keep the phase-1 keys ("id0".."id5") so a re-flash retains the
// cache state.
inline void saveCacheId(Preferences& prefs, int slot, const char* id) {
    char key[8];
    snprintf(key, sizeof key, "id%d", slot);
    if (prefs.getString(key, "") != id) prefs.putString(key, id);
}

inline bool loadCacheIds(Preferences& prefs, char ids[SCREEN_SLOTS][17]) {
    bool any = false;
    for (int slot = 0; slot < SCREEN_SLOTS; ++slot) {
        char key[8];
        snprintf(key, sizeof key, "id%d", slot);
        const String id = prefs.getString(key, "");
        strlcpy(ids[slot], id.c_str(), 17);
        any = any || ids[slot][0];
    }
    return any;
}

// The selection keeps the phase-1 keys ("screen", "spare-cursor").
inline void saveSelection(Preferences& prefs, int screenIndex, int spareCursor) {
    if (prefs.getUChar("screen", 255) != uint8_t(screenIndex))
        prefs.putUChar("screen", uint8_t(screenIndex));
    if (prefs.getUChar("spare-cursor", 0) != uint8_t(spareCursor))
        prefs.putUChar("spare-cursor", uint8_t(spareCursor));
}

// Load the persisted selection (defaults: art, spare0). Returns false
// when nothing was stored yet.
inline bool loadSelection(Preferences& prefs, int& screenIndex,
                          int& spareCursor) {
    const bool stored = prefs.isKey("screen");
    screenIndex = int(prefs.getUChar("screen", 0));
    spareCursor = int(prefs.getUChar("spare-cursor", 0));
    return stored;
}

}  // namespace lcd
