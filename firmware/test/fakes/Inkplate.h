// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
#pragma once
#include "Arduino.h"
#include <vector>
#include <functional>
inline std::function<void()> displayHook;
#if VENTANAL_PWRENTEST
#include <driver/gpio.h>
#endif
#define PROGMEM
constexpr uint8_t INKPLATE_BLACK=0, INKPLATE_WHITE=1, INKPLATE_YELLOW=2, INKPLATE_RED=3;
struct GFXglyph { uint16_t bitmapOffset; uint8_t width,height,xAdvance; int8_t xOffset,yOffset; };
struct GFXfont { uint8_t* bitmap; GFXglyph* glyph; uint16_t first,last; uint8_t yAdvance; };
class Inkplate {
public:
    std::vector<uint8_t> pixels = std::vector<uint8_t>(1920000, 255);
    struct FakeRTC {
        bool isSet() { return false; }
        uint32_t getEpoch() { return 0; }
        void setEpoch(uint32_t) {}
    } rtc;
    void fillScreen(uint8_t c) { std::fill(pixels.begin(), pixels.end(), c); }
    float voltage=3.92f;
    unsigned refreshes=0;
    unsigned begins=0;
    void begin() {
#if VENTANAL_PWRENTEST
        assert(!pwrHeld);
#endif
        ++begins;
    }
    void display() { ++refreshes; if(displayHook) displayHook(); }
    float readBattery() { return voltage; }
    int getRotation() { return rotation; }
    int rotation=0;
    void setRotation(int r) { rotation=r; }
    int width() { return rotation%2 ? 1600 : 1200; }
    int height() { return rotation%2 ? 1200 : 1600; }
    void drawPixel(int x,int y,uint8_t idx) { pixels.at(y*1600+x)=idx; }
};
