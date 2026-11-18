// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace lcd {

// Settings has long stretches of flat colour. Store each row independently
// so the RGB callback can refill any bounce buffer without a decoder cursor.
template<int Width, int Height, size_t Words>
struct LocalScene {
    static_assert(Width > 0 && Width < 0x8000 && Height > 0);

    uint16_t data[Words];
    uint32_t rows[Height + 1];

    bool encode(const uint16_t* pixels) {
        return encodeBand(pixels, 0, Height);
    }

    bool encodeBand(const uint16_t* pixels, int first, int count) {
        size_t used = first ? rows[first] : 0;
        for (int y = first; y < first + count; ++y, pixels += Width) {
            rows[y] = used;
            for (int x = 0; x < Width;) {
                int end = runEnd(pixels, x);
                if (end - x >= 3) {
                    if (used + 2 > Words) return false;
                    data[used++] = uint16_t(end - x);
                    data[used++] = pixels[x];
                } else {
                    while (end < Width) {
                        const int next = runEnd(pixels, end);
                        if (next - end >= 3) break;
                        end = next;
                    }
                    if (used + 1 + end - x > Words) return false;
                    data[used++] = uint16_t(0x8000 | (end - x));
                    memcpy(data + used, pixels + x, (end - x) * sizeof(*pixels));
                    used += end - x;
                }
                x = end;
            }
        }
        rows[first + count] = used;
        return true;
    }

    bool copyBand(const LocalScene& active, int first, int count) {
        size_t used = first ? rows[first] : 0;
        const auto length = active.rows[first + count] - active.rows[first];
        if (used + length > Words) return false;
        memcpy(data + used, active.data + active.rows[first], length * sizeof(data[0]));
        for (int row = first; row <= first + count; ++row)
            rows[row] = used + active.rows[row] - active.rows[first];
        return true;
    }

    __attribute__((always_inline)) inline void decode(uint16_t* out, int first, int count) const {
        const auto* in = data + rows[first];
        const auto* end = data + rows[first + count];
        while (in < end) {
            const unsigned header = *in++;
            const unsigned length = header & 0x7fff;
            if (header & 0x8000) {
                memcpy(out, in, length * sizeof(*out));
                out += length;
                in += length;
            } else {
                const uint16_t colour = *in++;
                if (!colour) {
                    memset(out, 0, length * sizeof(*out));
                    out += length;
                } else {
                    for (unsigned n = 0; n < length; ++n) *out++ = colour;
                }
            }
        }
    }

private:
    static int runEnd(const uint16_t* pixels, int first) {
        int end = first + 1;
        while (end < Width && pixels[end] == pixels[first]) ++end;
        return end;
    }
};

using LocalPage = LocalScene<800, 480, 48 * 1024>;

}  // namespace lcd
