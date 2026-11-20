// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
#pragma once

// LCD status overlays: the small OFFLINE mark and the boot waiting screen,
// drawn directly into an 800x480 RGB565 staging buffer. Text uses the
// generated LCD-only 4-bit Antonio/Inter coverage tables.

#include <cstdint>
#include <cstring>
#include <initializer_list>

#include "../common/staleness.h"
#include "frame.h"
#include "scanout.h"
#include "aafont.h"
#include "battery_lcd.h"
#include "readout_fonts.h"
#include <cstdio>
#include <cmath>
#include <cctype>

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

// Shared with native server geometry. Region buffers are packed, never
// panel-stride slices: clipping makes writes outside these rectangles impossible.
using ReadoutRect = ScanRect;
constexpr int TOPBAR_H = 26;
constexpr ReadoutRect CLOCK_GAP{548,0,204,26};
constexpr ReadoutRect ART_CORNER{550,0,250,80};
inline bool isLcarsSlot(int slot) { return slot==1 || slot==2; }
constexpr ReadoutRect WEATHER_CLOCK_GAP{CLOCK_GAP.x, CLOCK_GAP.y, CLOCK_GAP.w, 42};
inline ReadoutRect readoutRect(int slot, int weatherAge = -1) {
    if (!isLcarsSlot(slot)) return ART_CORNER;
    return slot == 1 && weatherAge >= 1 ? WEATHER_CLOCK_GAP : CLOCK_GAP;
}
inline uint16_t batteryColour(BatteryLevel lvl) {
    return lvl==BatteryLevel::Ok ? rgb565(95,174,79)
         : rgb565(221,68,68);
}
inline void renderReadout(uint16_t* region, const uint16_t* artRegion, bool lcars,
                          const char* time, const char* date, int percent, BatteryLevel lvl,
                          int weatherAge = -1) {
    const auto rect = readoutRect(lcars ? 1 : 0, weatherAge);
    const int w = rect.w, h = rect.h;
    if(lcars) memset(region,0,size_t(w)*CLOCK_GAP.h*2);
    else if (region != artRegion) memcpy(region,artRegion,size_t(w)*h*2);
    if(!*time) return; // no shade, date or fake 1970 clock while invalid
    const bool battery=percent>=0 && lvl!=BatteryLevel::None;
    char pct[8]="";
    if(battery) snprintf(pct,sizeof pct,"%d%%",percent);
    if(lcars) {
        const int dateX=6+textWidth(ClockTime,time)+10;
        drawText(region,w,h,ClockTime,time,6,20,rgb565(255,170,0));
        const auto dateColour = rgb565(153,150,186);
        drawText(region,w,h,ClockDate,date,dateX,18,dateColour);
        if (weatherAge >= 1) {
            char age[32];
            snprintf(age, sizeof age, "WEATHER %d H AGO", weatherAge);
            drawText(region, w, h, ClockDate, age, dateX, 39, dateColour);
        }
        if(battery) drawText(region,w,h,ClockDate,pct,w-6-textWidth(ClockDate,pct),18,batteryColour(lvl));
    } else {
        // Elliptical radial feather: 62% black at top-right, fully clear
        // at the left/bottom edge; RGB565 channels darken independently.
        for(int y=0;y<h;++y) for(int x=0;x<w;++x) {
            float radius=std::sqrt(float((w-1-x)*(w-1-x))/(w*w)+float(y*y)/(h*h));
            float shade=radius<0.72f ? 0.62f*(1-radius/0.72f) : 0;
            uint16_t c=region[y*w+x];
            region[y*w+x]=(int(((c>>11)&31)*(1-shade))<<11)
                         |(int(((c>>5)&63)*(1-shade))<<5)|int((c&31)*(1-shade));
        }
        char mixed[24]; snprintf(mixed,sizeof mixed,"%s",date);
        for(size_t i=0;mixed[i];++i)
            if(i && mixed[i-1]!=' ' && mixed[i]>='A' && mixed[i]<='Z') mixed[i]=char(std::tolower(mixed[i]));
        char line[40]; snprintf(line,sizeof line,"%s%s%s",mixed,battery?"   ":"",pct);
        drawText(region,w,h,ArtTime,time,w-14-textWidth(ArtTime,time),29,RGB565_WHITE);
        drawText(region,w,h,ArtDate,line,w-14-textWidth(ArtDate,line),46,RGB565_WHITE);
        if(battery) { // middle dot, outside the ASCII font table
            int dot=w-14-textWidth(ArtDate,pct)-5;
            setPixel(region,w,h,dot,42,RGB565_WHITE);
            setPixel(region,w,h,dot,43,RGB565_WHITE);
        }
    }
}
inline void pasteReadout(uint16_t* px, const uint16_t* region, ReadoutRect r) {
    for(int y=0;y<r.h;++y) memcpy(px+(r.y+y)*WIDTH+r.x,region+y*r.w,size_t(r.w)*2);
}
// Small OFFLINE mark, top-left, mirroring the e-paper readout wording.
inline void drawOfflineMark(uint16_t* pixels, int width, int height) {
    const int left = 8;
    int top = 8;
    const int line = 30;
    for (const char* text : {"OFFLINE", "NO HOST"}) {
        const int extent = textWidth(StatusFont, text);
        for (int y = top - 4; y < top + 24; ++y)
            for (int x = left - 6; x < left + extent + 6; ++x)
                setPixel(pixels, width, height, x, y, RGB565_BLACK);
        drawText(pixels, width, height, StatusFont, text, left, top + 20,
                 RGB565_RED);
        top += line;
    }
}

// --- Refresh feedback label ---------------------------------------------
//
// Long-press feedback: a refresh gesture shows a small, legible label in
// the overlay corner immediately (a refresh used to look like it did
// nothing when the server had nothing new). Same overlay font/style as
// the OFFLINE mark; removed by redrawing the cached frame.

using ventanal::RefreshLabel;

constexpr uint32_t REFRESH_LABEL_HOLD_MS = 2000;   // ~2 s

inline const char* refreshLabelText(RefreshLabel label) {
    switch (label) {
    case RefreshLabel::Refreshing: return "REFRESHING...";
    case RefreshLabel::UpToDate: return "UP TO DATE";
    case RefreshLabel::Offline: return "OFFLINE";
    case RefreshLabel::Failed: return "DOWNLOAD FAILED";
    default: break;
    }
    return nullptr;
}

// One label badge, bottom-left overlay corner (the OFFLINE mark owns the
// top-left; the live clock owns the top-right).
inline void drawRefreshBadge(uint16_t* pixels, int width, int height,
                             const char* text, uint16_t colour, int baseline) {
    const int extent = textWidth(StatusFont, text);
    const int left = 8;
    for (int y = baseline - 32; y <= baseline + 5; ++y)
        for (int x = left - 6; x < left + extent + 6; ++x)
            setPixel(pixels, width, height, x, y, RGB565_BLACK);
    drawText(pixels, width, height, StatusFont, text, left, baseline,
             colour);
}

inline void drawRefreshLabel(uint16_t* pixels, int width, int height,
                             const char* text, uint16_t colour) {
    drawRefreshBadge(pixels, width, height, text, colour, height - 14);
}

// Label state machine (host-pinned by test_lcd43b_overlay): begin() on the
// gesture, finish() when the refresh worker's pass is applied, tick() in
// loop() expires the ~2 s hold so the caller redraws the cached frame clean.
class RefreshLabelState {
public:
    void begin(uint32_t nowMs) {
        manual_ = true;
        label_ = RefreshLabel::Refreshing;
        sinceMs_ = nowMs;
    }
    bool manual() const { return manual_; }

    void finish(uint32_t nowMs, bool offline, bool changed, bool incomplete) {
        label_ = !manual_ ? RefreshLabel::None
                : offline ? RefreshLabel::Offline
                : changed || incomplete ? RefreshLabel::None
                                        : RefreshLabel::UpToDate;
        manual_ = false;
        sinceMs_ = nowMs;
    }
    void clear() {
        manual_ = false;
        label_ = RefreshLabel::None;
    }
    // True when held completion feedback expires.
    bool tick(uint32_t nowMs) {
        if (label_ != RefreshLabel::UpToDate && label_ != RefreshLabel::Offline)
            return false;
        if (nowMs - sinceMs_ < REFRESH_LABEL_HOLD_MS) return false;
        clear();
        return true;
    }
    RefreshLabel label(uint32_t epochNow, const ventanal::ContentStaleness& content) const {
        const auto policy = content.label(manual_, epochNow);
        return manual_ || policy == RefreshLabel::Failed ? policy : label_;
    }

private:
    bool manual_ = false;
    RefreshLabel label_ = RefreshLabel::None;
    uint32_t sinceMs_ = 0;
};

// Boot waiting screen: black panel with the host URL, like the e-paper's.
inline void drawWaiting(uint16_t* pixels, int width, int height, const char* url, bool configured = true,
                        int firstRow = 0, int fullHeight = 0) {
    memset(pixels, 0, size_t(width) * height * 2);
    drawText(pixels, width, height, WaitingTitle, configured ? "Waiting for host" : "Not set up", 12, 60 - firstRow,
             RGB565_WHITE);
    const auto& f = WaitingBody;
    int y = 100;
    if (!fullHeight) fullHeight = height;
    while (*url && y < fullHeight - 4) {
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
        drawText(pixels, width, height, f, line, 12, y - firstRow, RGB565_WHITE);
        url += n;
        y += 28;
    }
}

}  // namespace lcd