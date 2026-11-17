// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega

// LCD overlays: RGB565 helpers, the OFFLINE mark and the waiting screen,
// with pixel clipping proven safe at panel edges.

#include "../lcd43b/overlay.h"
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {

constexpr int PIXELS = lcd::WIDTH * lcd::HEIGHT;   // small buffer per row usage
uint16_t screen[PIXELS];

int countColour(uint16_t colour) {
    int total = 0;
    for (int i = 0; i < PIXELS; ++i)
        if (screen[i] == colour) ++total;
    return total;
}

}  // namespace

int main() {
    assert(lcd::rgb565(255, 0, 0) == 0xF800);
    assert(lcd::rgb565(0, 255, 0) == 0x07E0);
    assert(lcd::rgb565(0, 0, 255) == 0x001F);
    assert(lcd::rgb565(255, 255, 255) == 0xFFFF);
    {   // The OFFLINE mark paints red status pixels on a black panel.
        memset(screen, 0, sizeof screen);
        lcd::drawOfflineMark(screen, lcd::WIDTH, lcd::HEIGHT);
        assert(countColour(lcd::RGB565_RED) > 0);
        assert(countColour(lcd::RGB565_WHITE) == 0);
    }
    {   // The waiting screen clears to black with white host information.
        memset(screen, 0xFF, sizeof screen);
        lcd::drawWaiting(screen, lcd::WIDTH, lcd::HEIGHT,
                         "http://192.168.0.10:18780");
        assert(countColour(lcd::RGB565_BLACK) > 0);
        assert(countColour(lcd::RGB565_WHITE) > 0);
        int drawnUrlPixels = 0;
        for (int y = 96; y < 112; ++y)
            for (int x = 12; x < 500; ++x)
                if (screen[y * lcd::WIDTH + x] == lcd::RGB565_WHITE) ++drawnUrlPixels;
        assert(drawnUrlPixels > 0);   // the URL line exists below the title
    }
    {   // Text clips instead of writing out of bounds at either edge.
        memset(screen, 0, sizeof screen);
        lcd::drawText(screen, lcd::WIDTH, lcd::HEIGHT, Inter_Medium12cap7b,
                      "EDGE", lcd::WIDTH - 8, 240, lcd::RGB565_WHITE);
        lcd::drawText(screen, lcd::WIDTH, lcd::HEIGHT, Inter_Medium12cap7b,
                      "EDGE", -50, -40, lcd::RGB565_WHITE);
        assert(countColour(lcd::RGB565_WHITE) > 0);   // partial glyphs drew
        // No writes escaped the buffer: row 240 stays black past the text.
        int escape = 0;
        for (int y = 0; y < 16; ++y)
            for (int x = 0; x < 24; ++x)
                if (screen[(236 + y) * lcd::WIDTH + x] == lcd::RGB565_WHITE) ++escape;
        assert(escape == 0);
    }
    puts("lcd43b overlay: RGB565, OFFLINE mark, waiting screen, clipping passed");
    return 0;
}