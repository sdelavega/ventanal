// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
#pragma once

#include <cstdint>

namespace lcd::local {

struct SeenRing {
    uint32_t keys[64]{};
    uint8_t next = 0, count = 0;
    bool contains(uint32_t key) const {
        for (int i = 0; i < count; ++i)
            if (keys[i] == key) return true;
        return false;
    }
    void add(uint32_t key) {
        if (contains(key)) return;
        keys[next] = key;
        next = uint8_t((next + 1) % 64);
        if (count < 64) ++count;
    }
};

struct ArtState {
    uint32_t day = 0;          // YYYYMMDD of the set on screen
    uint32_t failedAt = 0;     // epoch of the last failed attempt
    SeenRing seen;
};

}  // namespace lcd::local
