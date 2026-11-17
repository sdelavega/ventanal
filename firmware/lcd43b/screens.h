// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
#pragma once

// Screen-list indexing for the lcd43b sketch. The device manifest always
// publishes art and weather, adds solar only while the ham gate passes, and
// may add up to three spare artworks. Screen indices run over that list:
//   art=0, weather=1, [solar=2 when published], then the spares.
// When solar is NOT published the first spare takes index 2 — index 2 must
// never map to the solar cache slot, which would show a phantom empty
// screen and hide spare0 (pinned by test_lcd43b_screens).

#include <cstdint>
#include "settings.h"

namespace lcd {

// Cache slots: 0=art, 1=weather, 2=solar, 3..5=spare0..spare2.
constexpr int SCREEN_SLOTS = 6;

inline int screenCount(bool solarPresent, int spareCount) {
    return 2 + (solarPresent ? 1 : 0) + spareCount;
}

inline int spareBase(bool solarPresent) {
    return 2 + (solarPresent ? 1 : 0);
}

// THE explicit active-slot list: cache slot backing each screen index, in
// order (art, weather, solar while published, then the spares). Every
// index-to-slot mapping goes through this one list so no code path can
// disagree with another about which cache backs a screen (pinned by
// test_lcd43b_screens). Returns the list length; entries beyond capacity
// are dropped.
inline int activeSlots(bool solarPresent, int spareCount, const DeviceSettings& settings,
                       int* slots, int capacity) {
    int count = 0;
    if (capacity <= 0) return 0;
    const bool fallback=!settings.showArt && !settings.showWeather && !(solarPresent && settings.showSolar);
    if (settings.showArt || fallback) slots[count++] = 0;
    if ((settings.showWeather || fallback) && count < capacity) slots[count++] = 1;
    if (solarPresent && (settings.showSolar || fallback) && count < capacity) slots[count++] = 2;
    for (int i = 0; i < spareCount && i < 3 && (settings.showArt || fallback) && count < capacity; ++i)
        slots[count++] = 3 + i;
    return count;
}

inline int activeSlots(bool solarPresent, int spareCount, int* slots, int capacity) {
    return activeSlots(solarPresent,spareCount,DeviceSettings{},slots,capacity);
}
inline int screenSlot(int index, bool solarPresent, int spareCount, const DeviceSettings& s) {
    int slots[SCREEN_SLOTS];
    const int n=activeSlots(solarPresent,spareCount,s,slots,SCREEN_SLOTS);
    return index>=0 && index<n ? slots[index] : -1;
}
// Double-tap targets remain empty with Art disabled, including fallback.
inline int spareTargets(bool solarPresent, int spareCount, const DeviceSettings& s,
                        int* slots, int capacity) {
    if(!s.showArt) return 0;
    int active[SCREEN_SLOTS], n=activeSlots(solarPresent,spareCount,s,active,SCREEN_SLOTS), count=0;
    for(int i=0;i<n && count<capacity;++i) if(active[i]>=3) slots[count++]=i;
    return count;
}
// Preserve cache-slot identity across visibility/publication changes; if hidden
// or removed select the first active screen. Also normalise double-tap cursor.
inline void remapSelection(int& index, int& cursor, int previousSlot,
                           bool solarPresent, int spareCount, const DeviceSettings& s) {
    int slots[SCREEN_SLOTS];
    const int n=activeSlots(solarPresent,spareCount,s,slots,SCREEN_SLOTS);
    index=0;
    for(int i=0;i<n;++i) if(slots[i]==previousSlot) index=i;
    const int targets=spareTargets(solarPresent,spareCount,s,slots,SCREEN_SLOTS);
    if(cursor<0 || cursor>=targets) cursor=0;
}

// Cache slot (0..SCREEN_SLOTS-1) for a screen index; negative when the
// index is outside the published list.
inline int screenSlot(int index, bool solarPresent, int spareCount) {
    int slots[SCREEN_SLOTS];
    const int count = activeSlots(solarPresent, spareCount, slots,
                                  SCREEN_SLOTS);
    if (index < 0 || index >= count) return -1;
    return slots[index];
}

// Spare ordinal (0..spareCount-1) for a screen index; negative when the
// index is not a spare screen.
inline int screenSpare(int index, bool solarPresent, int spareCount) {
    if (index < spareBase(solarPresent) || index >= screenCount(solarPresent, spareCount))
        return -1;
    return index - spareBase(solarPresent);
}

// Normalise a persisted selection against the current screen list: a
// screen index outside the list self-heals to art (the round-1 rule), and
// a spare cursor past the (possibly shrunk) spare list resets to spare0.
// Call after loading NVS state and after accepting a manifest.
inline void normalizeSelection(int& screenIndex, int& spareCursor,
                               bool solarPresent, int spareCount) {
    const int count = screenCount(solarPresent, spareCount);
    if (count <= 0) {
        screenIndex = 0;
        spareCursor = 0;
        return;
    }
    if (screenIndex < 0 || screenIndex >= count) screenIndex = 0;
    if (spareCursor < 0 || spareCursor >= spareCount) spareCursor = 0;
}

}  // namespace lcd
