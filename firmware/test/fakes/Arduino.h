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

constexpr int HIGH=1, LOW=0, OUTPUT=1, INPUT_PULLUP=2;
inline int fakeBusy=1;
inline std::function<int(int)> digitalReader;
inline int digitalRead(int pin) { return digitalReader?digitalReader(pin):(pin==18?HIGH:fakeBusy); }
inline int panelPower=1;
inline void pinMode(int,int) {}
inline void digitalWrite(int pin,int level) { if(pin==21) panelPower=level; }
