// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
#pragma once

// Tap rotation contains only art, weather and published solar. Spare cache
// slots remain available to the art override and refresh downloader.

#include <cstdint>
#include "settings.h"

namespace lcd {

// Cache slots: 0=art, 1=weather, 2=solar, 3..5=spare0..spare2.
constexpr int SCREEN_SLOTS = 6;

inline int activeSlots(bool solarPresent, int spareCount, const DeviceSettings& settings,
                       int* slots, int capacity) {
    (void)spareCount;
    int count = 0;
    if (capacity <= 0) return 0;
    const bool fallback=!settings.showArt && !settings.showWeather && !(solarPresent && settings.showSolar);
    if (settings.showArt) slots[count++] = 0;
    if ((settings.showWeather || fallback) && count < capacity) slots[count++] = 1;
    if (solarPresent && (settings.showSolar || fallback) && count < capacity) slots[count++] = 2;
    return count;
}

inline int screenSlot(int index, bool solarPresent, int spareCount, const DeviceSettings& s) {
    int slots[SCREEN_SLOTS];
    const int n=activeSlots(solarPresent,spareCount,s,slots,SCREEN_SLOTS);
    return index>=0 && index<n ? slots[index] : -1;
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
    (void)cursor; // OwnerState owns the art cursor; rotation never changes it.
}

}  // namespace lcd
