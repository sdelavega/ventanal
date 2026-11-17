// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
#pragma once
#include <cstdint>
#include <cstdio>
#include <string>
#include <functional>
#include <algorithm>
using String = std::string;
inline uint32_t fakeClock = 0;
inline uint32_t millis() { return fakeClock++; }
inline void delay(unsigned n) { fakeClock += n; }
struct SerialFake {
    std::string output;
    void begin(int) {} void flush() {}
    void println(const char* text) { output+=text; output+='\n'; }
    template<class... T> void printf(const char* format, T... values) {
        if constexpr(sizeof...(values)==0) output+=format;
        else { char buffer[512]; std::snprintf(buffer,sizeof buffer,format,values...); output+=buffer; }
    }
    size_t write(const uint8_t* bytes, size_t length) {
        output.append(reinterpret_cast<const char*>(bytes), length);
        return length;
    }
    explicit operator bool() const { return true; }
};
inline SerialFake Serial;

constexpr int HIGH=1, LOW=0, INPUT=0, OUTPUT=1, INPUT_PULLUP=2;
inline int fakeBusy=1;
inline std::function<int(int)> digitalReader;
inline int digitalRead(int pin) { return digitalReader?digitalReader(pin):(pin==18?HIGH:fakeBusy); }
inline int panelPower=1;
// Per-pin mode/level tracking so tests can model pins the way silicon
// samples them (e.g. the GT911 samples its INT pin level while RST
// rises). Modes/levels default to INPUT/LOW like a reset ESP32 GPIO.
inline int fakePinModes[64] = {};
inline int fakePinLevels[64] = {};
inline int fakePinMode(int pin) { return pin>=0&&pin<64 ? fakePinModes[pin] : INPUT; }
inline int fakePinLevel(int pin) { return pin>=0&&pin<64 ? fakePinLevels[pin] : LOW; }
inline void pinMode(int pin,int mode) { if(pin>=0&&pin<64) fakePinModes[pin]=mode; }
inline void digitalWrite(int pin,int level) {
    if(pin==21) panelPower=level;
    if(pin>=0&&pin<64) fakePinLevels[pin]=level;
}
