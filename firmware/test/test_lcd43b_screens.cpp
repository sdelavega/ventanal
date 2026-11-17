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
#include <initializer_list>

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
    {   // The ONE explicit active-slot list: index i maps to slots[i], so
        // every screen lookup agrees about which cache slot backs it.
        for (bool solar : {false, true}) {
            for (int spares = 0; spares <= 3; ++spares) {
                int slots[lcd::SCREEN_SLOTS];
                const int count = lcd::activeSlots(solar, spares, slots,
                                                   lcd::SCREEN_SLOTS);
                assert(count == lcd::screenCount(solar, spares));
                for (int index = 0; index < count; ++index)
                    assert(lcd::screenSlot(index, solar, spares) == slots[index]
                           && mapsToValidSlot(index, solar, spares));
                assert(lcd::activeSlots(solar, spares, slots, 0) == 0);
                assert(lcd::activeSlots(solar, spares, slots, 2) == 2);
            }
        }
    }
    {   // Persisted selection normalises to the current list: an index past
        // the end self-heals to art (the round-1 rule), a stale spare cursor
        // to spare0.
        int index = 5, cursor = 2;
        lcd::normalizeSelection(index, cursor, true, 3);
        assert(index == 5 && cursor == 2);      // solar+3 spares: still valid
        lcd::normalizeSelection(index, cursor, false, 3);   // solar dropped
        assert(index == 0 && cursor == 2);      // 5 past count 5 -> art
        lcd::normalizeSelection(index, cursor, true, 3);
        index = 3;
        lcd::normalizeSelection(index, cursor, false, 1);  // spares shrunk
        assert(index == 0 && cursor == 0);      // 3 past count 3 -> art
        index = 1;
        lcd::normalizeSelection(index, cursor, false, 0);  // no spares left
        assert(index == 1 && cursor == 0);      // weather still in range
        index = 9;
        lcd::normalizeSelection(index, cursor, false, 0);
        assert(index == 0);                     // far out of range -> art
        index = -3;
        lcd::normalizeSelection(index, cursor, true, 2);
        assert(index == 0);
        cursor = 5;
        lcd::normalizeSelection(index, cursor, true, 2);
        assert(cursor == 0);
        cursor = -1;
        lcd::normalizeSelection(index, cursor, true, 2);
        assert(cursor == 0);
    }
    {
        auto s=lcd::defaultSettings(false); s.showArt=false;
        int slots[6];
        assert(lcd::activeSlots(true,3,s,slots,6)==2 && slots[0]==1 && slots[1]==2);
        assert(lcd::spareTargets(true,3,s,slots,6)==0);
        s.showWeather=false; // only Solar enabled, then licence vanishes
        assert(lcd::activeSlots(false,3,s,slots,6)==5 && slots[0]==0 && slots[1]==1);
        assert(lcd::spareTargets(false,3,s,slots,6)==0);
        s.showWeather=true;
        int index=0,cursor=0;
        lcd::remapSelection(index,cursor,0,false,3,s);
        assert(lcd::screenSlot(index,false,3,s)==1);
        index=1; s.showArt=true;
        lcd::remapSelection(index,cursor,4,false,3,s);
        assert(lcd::screenSlot(index,false,3,s)==4); // retain visible slot
    }
    puts("lcd43b screens: active-slot list, normalisation, bounds passed");
    return 0;
}
