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

inline int screenSlot(int index, bool solarPresent, int spareCount, const DeviceSettings& s) {
    int slots[SCREEN_SLOTS];
    const int n=activeSlots(solarPresent,spareCount,s,slots,SCREEN_SLOTS);
    return index>=0 && index<n ? slots[index] : -1;
}
// Derive spare targets from the same effective visibility (including fallback).
inline int spareTargets(bool solarPresent, int spareCount, const DeviceSettings& s,
                        int* slots, int capacity) {
    int active[SCREEN_SLOTS], n=activeSlots(solarPresent,spareCount,s,active,SCREEN_SLOTS), count=0;
    for(int i=0;i<n && count<capacity;++i) if(active[i]>=3) slots[count++]=i;
    return count;
}
// Preserve cache-slot identity across visibility/publication changes; if hidden
// or removed select the nearest active cache slot. Also normalise double-tap cursor.
inline void remapSelection(int& index, int& cursor, int previousSlot,
                           bool solarPresent, int spareCount, const DeviceSettings& s) {
    int slots[SCREEN_SLOTS];
    const int n=activeSlots(solarPresent,spareCount,s,slots,SCREEN_SLOTS);
    index=0;
    int distance=SCREEN_SLOTS+1;
    for(int i=0;i<n;++i) {
        const int d=slots[i]>previousSlot?slots[i]-previousSlot:previousSlot-slots[i];
        if(d<distance) { distance=d; index=i; }
    }
    const int targets=spareTargets(solarPresent,spareCount,s,slots,SCREEN_SLOTS);
    if(cursor<0 || cursor>=targets) cursor=0;
}

}  // namespace lcd
