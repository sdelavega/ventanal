// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
#pragma once
#include <cstdint>
#include <cstddef>
#include <cstring>
namespace lcd {
struct DeviceSettings {
    uint8_t brightness = 100; // 25|50|75|100
    uint8_t timeoutMin = 0; // 1|5|15|0=never
    bool showArt = true, showWeather = true, showSolar = true;
    int batteryFullMv = 0;
};
inline DeviceSettings defaultSettings(bool powerDetectable) {
    DeviceSettings s; s.timeoutMin=powerDetectable ? 5 : 0; return s;
}
inline bool validSettings(const DeviceSettings& s) {
    return (s.brightness==25 || s.brightness==50 || s.brightness==75 || s.brightness==100)
        && (s.timeoutMin==0 || s.timeoutMin==1 || s.timeoutMin==5 || s.timeoutMin==15)
        && (s.showArt || s.showWeather || s.showSolar)
        && (s.batteryFullMv==0 || (s.batteryFullMv>=4000 && s.batteryFullMv<=4300));
}
// which: Art=0, Weather=1, Solar=2. Pass current publication state when
// editing; the three-argument form assumes all three are published.
inline bool setVisibility(DeviceSettings& s, int which, bool on, bool solarPresent=true) {
    if(which<0 || which>2) return false;
    DeviceSettings next=s;
    if(which==0) next.showArt=on;
    if(which==1) next.showWeather=on;
    if(which==2) next.showSolar=on;
    if(!next.showArt && !next.showWeather && !(solarPresent && next.showSolar)) return false;
    s=next; return true;
}
// Channel-wise integer scaling, truncating toward black. In-place is safe;
// callers scale staging pixels, never the cached frame or readout background.
inline void dimRgb565(const uint16_t* src, uint16_t* dst, size_t n, uint8_t percent) {
    if (percent >= 100) {
        if (src != dst) memcpy(dst, src, n * sizeof *src);
        return;
    }
    uint8_t scaled[64];
    for (unsigned level = 0; level < 64; ++level)
        scaled[level] = uint8_t(level * percent / 100);
    for(size_t i=0;i<n;++i) {
        const unsigned p=src[i];
        dst[i]=uint16_t((scaled[p>>11]<<11) | (scaled[(p>>5)&63]<<5) | scaled[p&31]);
    }
}
} // namespace lcd
