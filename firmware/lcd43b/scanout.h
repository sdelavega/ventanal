// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include "local_scene.h"

namespace lcd {

struct ScanRect {
    int x, y, w, h;
};

struct ScanPatch {
    ScanRect rect{};
    const uint16_t* pixels = nullptr;
};

struct ScanFrame {
    const uint16_t* pixels = nullptr;
    ScanPatch patches[3]{};
    unsigned brightnessQuarters = 4;
    const LocalPage* local = nullptr;
};

// Scale two packed RGB565 pixels. Separate the lowest two bits of each
// channel before multiplying so the 5/6-bit fields cannot carry into
// their neighbours. The final add restores floor(remainder * n / 4).
inline uint32_t dimScanPair(uint32_t pair, unsigned n) {
    constexpr uint32_t lowBits = 0x18631863;
    constexpr uint32_t carryBits = lowBits << 2;
    return (((pair & ~lowBits) >> 2) * n) +
           ((((pair & lowBits) * n) & carryBits) >> 2);
}

// IDF supplies a pixel offset. The configured buffer size is an integral
// number of rows and divides the entire frame.
__attribute__((always_inline)) inline void finishScanRows(uint16_t* out, const ScanFrame& frame,
                                                           int width, int offset, int bytes) {
    const int count = bytes / int(sizeof(*out));
    const int first = offset / width;
    const int end = first + count / width;
    for (const auto& patch : frame.patches) {
        if (!patch.pixels) continue;
        const auto r = patch.rect;
        const int from = std::max(first, r.y);
        const int to = std::min(end, r.y + r.h);
        for (int y = from; y < to; ++y)
            memcpy(out + (y - first) * width + r.x,
                   patch.pixels + (y - r.y) * r.w, r.w * sizeof(*out));
    }
    if (frame.brightnessQuarters < 4) {
        // The DMA buffer and every 800-pixel row are word-aligned.
        auto* words = static_cast<uint16_t*>(__builtin_assume_aligned(out, 4));
        for (int i = 0; i < count; i += 2) {
            uint32_t pair;
            memcpy(&pair, words + i, sizeof(pair));
            pair = dimScanPair(pair, frame.brightnessQuarters);
            memcpy(words + i, &pair, sizeof(pair));
        }
    }
}

}  // namespace lcd
