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

namespace lcd {

// Cache slots: 0=art, 1=weather, 2=solar, 3..5=spare0..spare2.
constexpr int SCREEN_SLOTS = 6;

inline int screenCount(bool solarPresent, int spareCount) {
    return 2 + (solarPresent ? 1 : 0) + spareCount;
}

inline int spareBase(bool solarPresent) {
    return 2 + (solarPresent ? 1 : 0);
}

// Cache slot (0..SCREEN_SLOTS-1) for a screen index; negative when the
// index is outside the published list.
inline int screenSlot(int index, bool solarPresent, int spareCount) {
    if (index < 0 || index >= screenCount(solarPresent, spareCount)) return -1;
    if (index < 2 || (solarPresent && index == 2)) return index;
    return 3 + (index - spareBase(solarPresent));
}

// Spare ordinal (0..spareCount-1) for a screen index; negative when the
// index is not a spare screen.
inline int screenSpare(int index, bool solarPresent, int spareCount) {
    if (index < spareBase(solarPresent) || index >= screenCount(solarPresent, spareCount))
        return -1;
    return index - spareBase(solarPresent);
}

}  // namespace lcd
