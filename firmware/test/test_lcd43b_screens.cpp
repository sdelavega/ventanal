// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega

// Screen-list indexing for the lcd43b sketch: art and weather are always
// published, solar only when the ham gate passes, then the spares. When
// solar is absent the first spare takes index 2 — index 2 must never map
// to the (unpublished) solar cache slot, which previously produced a
// phantom empty screen and made spare0 unreachable.

#include "../lcd43b/screens.h"

#include <cassert>
#include <cstdio>

namespace {

bool mapsToValidSlot(int index, bool solar, int spares) {
    const int slot = lcd::screenSlot(index, solar, spares);
    return slot >= 0 && slot < lcd::SCREEN_SLOTS;
}

}  // namespace

int main() {
    {   // Solar published: art, weather, solar, spare0..2 at slots 0..5.
        const bool solar = true;
        assert(lcd::screenCount(solar, 3) == 6);
        assert(lcd::spareBase(solar) == 3);
        for (int index = 0; index < 3; ++index) {
            assert(lcd::screenSlot(index, solar, 3) == index);
            assert(lcd::screenSpare(index, solar, 3) == -1);
        }
        assert(lcd::screenSlot(3, solar, 3) == 3);   // spare0
        assert(lcd::screenSlot(4, solar, 3) == 4);   // spare1
        assert(lcd::screenSlot(5, solar, 3) == 5);   // spare2
        assert(lcd::screenSpare(3, solar, 3) == 0);
        assert(lcd::screenSpare(4, solar, 3) == 1);
        assert(lcd::screenSpare(5, solar, 3) == 2);
    }
    {   // Solar absent: index 2 is the first spare, never the solar slot.
        const bool solar = false;
        assert(lcd::screenCount(solar, 3) == 5);
        assert(lcd::spareBase(solar) == 2);
        assert(lcd::screenSlot(0, solar, 3) == 0);   // art
        assert(lcd::screenSlot(1, solar, 3) == 1);   // weather
        assert(lcd::screenSlot(2, solar, 3) == 3);   // spare0, NOT the solar slot
        assert(lcd::screenSlot(3, solar, 3) == 4);   // spare1
        assert(lcd::screenSlot(4, solar, 3) == 5);   // spare2
        assert(lcd::screenSpare(2, solar, 3) == 0);
        assert(lcd::screenSpare(3, solar, 3) == 1);
        assert(lcd::screenSpare(4, solar, 3) == 2);
        assert(lcd::screenSpare(0, solar, 3) == -1);
        assert(lcd::screenSpare(1, solar, 3) == -1);
        // No valid index may map to the unpublished solar cache slot.
        for (int index = 0; index < lcd::screenCount(solar, 3); ++index)
            assert(lcd::screenSlot(index, solar, 3) != 2);
    }
    {   // Without spares the list is just art and weather.
        assert(lcd::screenCount(false, 0) == 2);
        assert(lcd::screenCount(true, 0) == 3);
        assert(lcd::screenSlot(2, false, 0) == -1);   // past the end
        assert(lcd::screenSlot(2, true, 0) == 2);     // solar itself
        assert(lcd::screenSlot(3, true, 0) == -1);
    }
    {   // Out-of-range and negative indices are rejected, in range are not.
        assert(lcd::screenSlot(-1, true, 3) == -1);
        assert(lcd::screenSlot(6, true, 3) == -1);
        assert(lcd::screenSlot(5, true, 3) == 5);
        assert(lcd::screenSpare(-1, true, 3) == -1);
        assert(lcd::screenSpare(6, true, 3) == -1);
        for (int index = 0; index < lcd::screenCount(true, 3); ++index)
            assert(mapsToValidSlot(index, true, 3));
        for (int index = 0; index < lcd::screenCount(false, 3); ++index)
            assert(mapsToValidSlot(index, false, 3));
    }
    puts("lcd43b screens: solar-absent indexing, spare mapping, bounds passed");
    return 0;
}
