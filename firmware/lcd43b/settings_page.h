// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
#pragma once
#include "settings.h"
#include "overlay.h"
#include "lcars_geometry.h"

namespace lcd {
enum class SettingsHit { None, Brightness25, Brightness50, Brightness75, Brightness100,
    Timeout1, Timeout5, Timeout15, TimeoutNever, ToggleArt, ToggleWeather, ToggleSolar,
    BatteryFull, Done };
struct SettingsControl { SettingsHit hit; ReadoutRect rect; const char* label; };
constexpr SettingsControl SETTINGS_CONTROLS[] = {
    {SettingsHit::Brightness25,{310,82,110,52},"25%"},
    {SettingsHit::Brightness50,{430,82,110,52},"50%"},
    {SettingsHit::Brightness75,{550,82,110,52},"75%"},
    {SettingsHit::Brightness100,{670,82,110,52},"100%"},
    {SettingsHit::Timeout1,{310,186,110,52},"1 MIN"},
    {SettingsHit::Timeout5,{430,186,110,52},"5 MIN"},
    {SettingsHit::Timeout15,{550,186,110,52},"15 MIN"},
    {SettingsHit::TimeoutNever,{670,186,110,52},"NEVER"},
    {SettingsHit::ToggleArt,{310,290,150,52},"ART"},
    {SettingsHit::ToggleWeather,{470,290,150,52},"WEATHER"},
    {SettingsHit::ToggleSolar,{630,290,150,52},"SOLAR"},
    {SettingsHit::BatteryFull,{150,374,430,52},"BATTERY IS FULL - SET AS 100%"},
    {SettingsHit::Done,{610,384,170,56},"DONE"}
};
inline bool settingsControlVisible(SettingsHit h, bool solarListed, bool batteryButton) {
    return (h!=SettingsHit::ToggleSolar || solarListed)
        && (h!=SettingsHit::BatteryFull || batteryButton);
}
inline SettingsHit hitSettings(int x,int y,bool solarListed,bool batteryButton) {
    for(const auto& c:SETTINGS_CONTROLS) {
        const auto r=c.rect;
        if(settingsControlVisible(c.hit,solarListed,batteryButton)
           && x>=r.x && x<r.x+r.w && y>=r.y && y<r.y+r.h) return c.hit;
    }
    return SettingsHit::None;
}
inline bool applySettingsHit(DeviceSettings& s,SettingsHit h,bool solarListed,int batteryMv) {
    switch(h) {
    case SettingsHit::Brightness25: s.brightness=25; return true;
    case SettingsHit::Brightness50: s.brightness=50; return true;
    case SettingsHit::Brightness75: s.brightness=75; return true;
    case SettingsHit::Brightness100: s.brightness=100; return true;
    case SettingsHit::Timeout1: s.timeoutMin=1; return true;
    case SettingsHit::Timeout5: s.timeoutMin=5; return true;
    case SettingsHit::Timeout15: s.timeoutMin=15; return true;
    case SettingsHit::TimeoutNever: s.timeoutMin=0; return true;
    case SettingsHit::ToggleArt: return setVisibility(s,0,!s.showArt,solarListed);
    case SettingsHit::ToggleWeather: return setVisibility(s,1,!s.showWeather,solarListed);
    case SettingsHit::ToggleSolar: return solarListed && setVisibility(s,2,!s.showSolar,solarListed);
    case SettingsHit::BatteryFull:
        if(batteryMv<4000 || batteryMv>4300) return false;
        s.batteryFullMv=batteryMv; return true;
    default: return false;
    }
}
struct SettingsSession {
    bool active=false;
    uint32_t lastTouch=0;
    void open(uint32_t now) { active=true; lastTouch=now; }
    void touch(uint32_t now) { lastTouch=now; }
    bool expired(uint32_t now) const { return active && uint32_t(now-lastTouch)>=30000; }
};
inline void settingsBox(uint16_t* px,ReadoutRect r,uint16_t colour,int radius=0) {
    for(int y=0;y<r.h;++y) for(int x=0;x<r.w;++x) {
        const int dx=x<radius?radius-1-x:(x>=r.w-radius?x-(r.w-radius):0);
        const int dy=y<radius?radius-1-y:(y>=r.h-radius?y-(r.h-radius):0);
        if(dx*dx+dy*dy<=radius*radius) setPixel(px,WIDTH,HEIGHT,r.x+x,r.y+y,colour);
    }
}
// Fonts are ASCII. Render the footer's UTF-8 middle dots explicitly.
inline void settingsFooter(uint16_t* px,const char* text,int x,int baseline,uint16_t colour) {
    while(*text) {
        if(static_cast<unsigned char>(text[0])==0xc2 && static_cast<unsigned char>(text[1])==0xb7) {
            settingsBox(px,{x+2,baseline-5,2,2},colour); x+=7; text+=2;
        } else {
            char one[2]={*text++,0};
            drawText(px,WIDTH,HEIGHT,ArtDate,one,x,baseline,colour);
            x+=textWidth(ArtDate,one);
        }
    }
}
// Shape-only entry point allows a whole-frame comparison against the server.
inline void drawSettingsChrome(uint16_t* px,bool labels=true) {
    memset(px,0,size_t(WIDTH)*HEIGHT*2);
    const auto orange=rgb565(255,119,0);
    for(const auto& span:LCARS_ELBOWS)
        for(int x=span.left;x<=span.right;++x) px[span.y*WIDTH+x]=orange;
    settingsBox(px,{534,0,14,26},rgb565(255,170,0));
    struct Section { int y,h; uint16_t colour; const char* label; };
    const Section sections[] = {
        {60,120,rgb565(221,187,255),"DISPLAY"},
        {184,100,rgb565(170,170,255),"POWER"},
        {288,138,rgb565(255,170,0),"SCREENS"}
    };
    for(const auto& section:sections) {
        settingsBox(px,{0,section.y,100,section.h},section.colour);
        if(labels) drawText(px,WIDTH,HEIGHT,SidebarLabel,section.label,
                           92-textWidth(SidebarLabel,section.label),section.y+section.h-6,0);
    }
}
inline void drawSettingsPage(uint16_t* px,const DeviceSettings& s,bool solarListed,
                             bool batteryButton,const char* footer) {
    const auto orange=rgb565(255,119,0), gold=rgb565(255,170,0);
    const auto sky=rgb565(170,170,255), light=rgb565(153,150,186);
    drawSettingsChrome(px);
    drawText(px,WIDTH,HEIGHT,Antonio_Regular18pt7b,"SETTINGS",150,63,gold);
    drawText(px,WIDTH,HEIGHT,ClockTime,"BRIGHTNESS",150,115,gold);
    drawText(px,WIDTH,HEIGHT,ClockTime,"SCREEN OFF AFTER",150,219,gold);
    drawText(px,WIDTH,HEIGHT,ClockTime,"SHOW",150,323,gold);
    drawText(px,WIDTH,HEIGHT,ArtDate,"Dims the picture; this board's backlight is on/off only.",310,151,light);
    drawText(px,WIDTH,HEIGHT,ArtDate,"Timeout applies on USB and battery alike. Touch wakes it.",310,255,light);
    drawText(px,WIDTH,HEIGHT,ArtDate,"At least one stays on. Hidden screens aren't downloaded.",310,359,light);
    for(const auto& c:SETTINGS_CONTROLS) {
        if(!settingsControlVisible(c.hit,solarListed,batteryButton)) continue;
        const int h=int(c.hit);
        const bool toggle=h>=int(SettingsHit::ToggleArt) && h<=int(SettingsHit::ToggleSolar);
        const bool selected=(h>=1 && h<=4 && s.brightness==h*25)
            || (c.hit==SettingsHit::Timeout1 && s.timeoutMin==1)
            || (c.hit==SettingsHit::Timeout5 && s.timeoutMin==5)
            || (c.hit==SettingsHit::Timeout15 && s.timeoutMin==15)
            || (c.hit==SettingsHit::TimeoutNever && s.timeoutMin==0)
            || (c.hit==SettingsHit::ToggleArt && s.showArt)
            || (c.hit==SettingsHit::ToggleWeather && s.showWeather)
            || (c.hit==SettingsHit::ToggleSolar && s.showSolar);
        const bool button=c.hit==SettingsHit::Done || c.hit==SettingsHit::BatteryFull;
        settingsBox(px,c.rect,button?orange:(selected?(toggle?orange:sky):rgb565(58,58,74)),c.rect.h/2);
        const auto& font=c.hit==SettingsHit::BatteryFull?ArtDate:ClockTime;
        const int x=c.rect.x+(c.rect.w-textWidth(font,c.label))/2;
        drawText(px,WIDTH,HEIGHT,font,c.label,x,c.rect.y+c.rect.h/2+8,selected||button?0:light);
        if(toggle && selected) { // visible check mark independent of font coverage
            const int cx=c.rect.x+c.rect.w-20,cy=c.rect.y+25;
            for(int i=0;i<5;++i) settingsBox(px,{cx+i,cy+i,2,2},0);
            for(int i=0;i<9;++i) settingsBox(px,{cx+4+i,cy+4-i,2,2},0);
        }
    }
    settingsFooter(px,footer,150,447,light);
}
} // namespace lcd
