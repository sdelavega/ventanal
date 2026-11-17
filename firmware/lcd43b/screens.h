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
inline int activeSlots(bool solarPresent, int spareCount, int* slots,
                       int capacity) {
    int count = 0;
    if (capacity <= 0) return 0;
    slots[count++] = 0;                                  // art
    if (count < capacity) slots[count++] = 1;            // weather
    if (solarPresent && count < capacity) slots[count++] = 2;
    for (int i = 0; i < spareCount && count < capacity; ++i)
        slots[count++] = 3 + i;
    return count;
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
