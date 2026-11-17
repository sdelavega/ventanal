// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
#pragma once

// Persisted cross-reboot state for the lcd43b sketch, kept in NVS under the
// phase-1 "ventanal" Preferences namespace:
//   - the validated quiet window (the manifest's quiet block, so an
//     offline reboot still honours quiet hours — pinned by
//     test_lcd43b_persist);
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

namespace lcd {

// "no quiet window stored" marker for the u16 keys.
constexpr uint16_t QUIET_NONE = 0xFFFF;

// Persist the (manifest-validated) quiet window; an invalid or absent
// window clears any previously stored one.
inline void saveQuiet(Preferences& prefs, const DeviceManifest& manifest) {
    if (manifest.quietValid()) {
        prefs.putUShort("quiet-start", uint16_t(manifest.quietStart));
        prefs.putUShort("quiet-end", uint16_t(manifest.quietEnd));
    } else {
        prefs.putUShort("quiet-start", QUIET_NONE);
        prefs.putUShort("quiet-end", QUIET_NONE);
    }
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
// can still normalise the selection against the right list.
inline void savePublishShape(Preferences& prefs, const DeviceManifest& m) {
    prefs.putUChar("solar-pub", m.screens[2].present ? 1 : 0);
    prefs.putUChar("spare-count", m.spareCount);
}

inline void loadPublishShape(Preferences& prefs, DeviceManifest& m) {
    m.screens[2].present = prefs.getUChar("solar-pub", 0) != 0;
    m.spareCount = uint8_t(prefs.getUChar("spare-count", 0) % (SPARE_SLOTS + 1));
}

// Cache ids keep the phase-1 keys ("id0".."id5") so a re-flash retains the
// cache state.
inline void saveCacheId(Preferences& prefs, int slot, const char* id) {
    char key[8];
    snprintf(key, sizeof key, "id%d", slot);
    prefs.putString(key, id);
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
    prefs.putUChar("screen", uint8_t(screenIndex));
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
