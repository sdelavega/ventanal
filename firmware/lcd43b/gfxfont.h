// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
#pragma once

// Adafruit-GFX font structures used by the bundled generated font tables.

#include <cstdint>

#ifndef PROGMEM
#define PROGMEM
#endif

struct GFXglyph {
    uint16_t bitmapOffset;
    uint8_t width, height, xAdvance;
    int8_t xOffset, yOffset;
};

struct GFXfont {
    const uint8_t* bitmap;
    const GFXglyph* glyph;
    uint8_t first, last;
    uint8_t yAdvance;
};