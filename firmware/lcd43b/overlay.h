// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
#pragma once

// LCD status overlays: the small OFFLINE mark and the boot waiting screen,
// drawn directly into an 800x480 RGB565 staging buffer. Text reuses the
// generated Antonio/Inter glyph tables shipped with the e-paper firmware.

#include <cstdint>
#include <cstring>
#include <initializer_list>

#include "frame.h"
#include "gfxfont.h"
#include "../ventanal/fonts/Antonio-Regular.h"
#include "../ventanal/fonts/Inter-Medium.h"

namespace lcd {

constexpr uint16_t RGB565_BLACK = 0x0000;
constexpr uint16_t RGB565_WHITE = 0xFFFF;
constexpr uint16_t RGB565_RED = 0xF800;
constexpr uint16_t RGB565_YELLOW = 0xFFE0;

inline uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
    return (uint16_t(r >> 3) << 11) | (uint16_t(g >> 2) << 5) | uint16_t(b >> 3);
}

inline void setPixel(uint16_t* pixels, int width, int height, int x, int y,
                     uint16_t colour) {
    if (x >= 0 && y >= 0 && x < width && y < height)
        pixels[size_t(y) * width + x] = colour;
}

inline int advance(const GFXfont& f, unsigned char c) {
    return c >= f.first && c <= f.last ? f.glyph[c - f.first].xAdvance : 0;
}

inline int textWidth(const GFXfont& f, const char* s) {
    int total = 0;
    for (; *s; ++s) total += advance(f, static_cast<unsigned char>(*s));
    return total;
}

// Draw one line at its natural size; never rescale font pixels.
inline void drawText(uint16_t* pixels, int width, int height, const GFXfont& f,
                     const char* s, int left, int baseline, uint16_t colour) {
    int pen = 0;
    for (; *s; ++s) {
        const auto c = static_cast<unsigned char>(*s);
        if (c >= f.first && c <= f.last) {
            const GFXglyph& g = f.glyph[c - f.first];
            for (int y = 0; y < g.height; ++y) {
                for (int x = 0; x < g.width; ++x) {
                    const int bit = y * g.width + x;
                    if (f.bitmap[g.bitmapOffset + bit / 8] & (0x80 >> (bit % 8)))
                        setPixel(pixels, width, height,
                                 left + pen + g.xOffset + x, baseline + g.yOffset + y,
                                 colour);
                }
            }
        }
        pen += advance(f, c);
    }
}

// Small OFFLINE mark, top-left, mirroring the e-paper readout wording.
inline void drawOfflineMark(uint16_t* pixels, int width, int height) {
    const int left = 8;
    int top = 8;
    const int line = 30;
    for (const char* text : {"OFFLINE", "NO HOST"}) {
        const int extent = textWidth(Antonio_Regular18pt7b, text);
        for (int y = top - 4; y < top + 24; ++y)
            for (int x = left - 6; x < left + extent + 6; ++x)
                setPixel(pixels, width, height, x, y, RGB565_BLACK);
        drawText(pixels, width, height, Antonio_Regular18pt7b, text, left, top + 20,
                 RGB565_RED);
        top += line;
    }
}

// Boot waiting screen: black panel with the host URL, like the e-paper's.
inline void drawWaiting(uint16_t* pixels, int width, int height, const char* url) {
    memset(pixels, 0, size_t(width) * height * 2);
    drawText(pixels, width, height, Inter_Medium15cap7b, "Waiting for host", 12, 60,
             RGB565_WHITE);
    const auto& f = Inter_Medium12cap7b;
    int y = 100;
    while (*url && y < height - 4) {
        char line[96];
        size_t n = 0;
        int used = 0;
        while (url[n] && n < sizeof line - 1 && used + advance(f, url[n]) <= width - 24) {
            used += advance(f, url[n]);
            line[n] = url[n];
            ++n;
        }
        if (!n) break;
        line[n] = '\0';
        drawText(pixels, width, height, f, line, 12, y, RGB565_WHITE);
        url += n;
        y += 28;
    }
}

}  // namespace lcd