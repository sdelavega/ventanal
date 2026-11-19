// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
#pragma once
#include "settings.h"
#include "overlay.h"
#include "lcars_geometry.h"
#include <cstdio>

namespace lcd {
enum class SettingsHit { None, Brightness25, Brightness50, Brightness75, Brightness100,
    Timeout1, Timeout5, Timeout15, TimeoutNever, ToggleArt, ToggleWeather, ToggleSolar,
    BatteryFull, Done, SetupWifi };
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
    {SettingsHit::BatteryFull,{150,374,210,52},"BATTERY IS FULL - SET AS 100%"},
    {SettingsHit::SetupWifi,{370,374,226,52},"SETUP WI-FI"},
    {SettingsHit::Done,{610,384,170,56},"DONE"}
};
inline ReadoutRect settingsDamage(SettingsHit hit) {
    for (const auto& control : SETTINGS_CONTROLS)
        if (control.hit == hit) return control.rect;
    return {};
}

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
// A band uses the same coordinates as the full Settings page. The renderer
// clips to it so a single small workspace can build either idle scan scene.
struct SettingsSurface {
    uint16_t* pixels;
    int first;
    int rows;

    SettingsSurface(uint16_t* pixels, int first = 0, int rows = HEIGHT)
        : pixels(pixels), first(first), rows(rows) {}

    void clear() const { memset(pixels, 0, size_t(WIDTH) * rows * sizeof(*pixels)); }
    void text(const AAFont& font, const char* label, int x, int baseline, uint16_t colour) const {
        drawText(pixels, WIDTH, rows, font, label, x, baseline - first, colour);
    }
};

inline void settingsBox(SettingsSurface px,ReadoutRect r,uint16_t colour,int radius=0) {
    for(int y=std::max(0,px.first-r.y);y<std::min(r.h,px.first+px.rows-r.y);++y) for(int x=0;x<r.w;++x) {
        unsigned coverage=16;
        if(radius) {
            coverage=0;
            for(int sy=0;sy<4;++sy) for(int sx=0;sx<4;++sx) {
                const int fx=x*8+sx*2+1,fy=y*8+sy*2+1;
                const int dx=fx<radius*8?radius*8-fx:(fx>(r.w-radius)*8?fx-(r.w-radius)*8:0);
                const int dy=fy<radius*8?radius*8-fy:(fy>(r.h-radius)*8?fy-(r.h-radius)*8:0);
                if(dx*dx+dy*dy<=radius*radius*64) ++coverage;
            }
        }
        const int dx=r.x+x,dy=r.y+y;
        if(coverage && dx>=0 && dx<WIDTH && dy>=0 && dy<HEIGHT) {
            auto& pixel=px.pixels[(dy-px.first)*WIDTH+dx];
            pixel=blendAA(pixel,colour,(coverage*15+8)/16);
        }
    }
}
// Fonts are ASCII. Render the footer's UTF-8 middle dots explicitly.
inline void settingsFooter(SettingsSurface px,const char* text,int x,int baseline,uint16_t colour) {
    while(*text) {
        if(static_cast<unsigned char>(text[0])==0xc2 && static_cast<unsigned char>(text[1])==0xb7) {
            settingsBox(px,{x+2,baseline-5,2,2},colour); x+=7; text+=2;
        } else {
            char one[2]={*text++,0};
            px.text(ArtDate,one,x,baseline,colour);
            x+=textWidth(ArtDate,one);
        }
    }
}
// Shape-only entry point allows a whole-frame comparison against the server.
inline void drawSettingsChrome(SettingsSurface px,bool labels=true) {
    px.clear();
    for(const auto& span:LCARS_CHROME)
        if (span.y >= px.first && span.y < px.first + px.rows)
            for(int x=span.left;x<=span.right;++x)
                px.pixels[(span.y-px.first)*WIDTH+x]=span.colour;
    if(labels) for(const auto& section:SETTINGS_SECTIONS)
        px.text(SidebarLabel,section.label,
                 92-textWidth(SidebarLabel,section.label),section.y+section.h-6,0);
}
inline void drawSettingsPage(SettingsSurface px,const DeviceSettings& s,bool solarListed,
                             bool batteryButton,const char* footer) {
    const auto orange=rgb565(255,119,0), gold=rgb565(255,170,0);
    const auto sky=rgb565(170,170,255), light=rgb565(153,150,186);
    drawSettingsChrome(px);
    px.text(StatusFont,"SETTINGS",150,63,gold);
    px.text(ClockTime,"BRIGHTNESS",150,115,gold);
    px.text(ClockTime,"SCREEN OFF AFTER",150,219,gold);
    px.text(ClockTime,"SHOW",150,323,gold);
    px.text(ArtDate,"Dims the picture; this board's backlight is on/off only.",310,151,light);
    px.text(ArtDate,"Timeout applies on USB and battery alike. Touch wakes it.",310,255,light);
    px.text(ArtDate,"At least one stays on. Hidden screens aren't downloaded.",310,359,light);
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
        const bool button=c.hit==SettingsHit::Done || c.hit==SettingsHit::BatteryFull
            || c.hit==SettingsHit::SetupWifi;
        settingsBox(px,c.rect,button?orange:(selected?(toggle?orange:sky):rgb565(58,58,74)),c.rect.h/2);
        const auto& font=c.hit==SettingsHit::BatteryFull?ArtDate:ClockTime;
        const int x=c.rect.x+(c.rect.w-textWidth(font,c.label))/2;
        px.text(font,c.label,x,c.rect.y+c.rect.h/2+8,selected||button?0:light);
        if(toggle && selected) { // visible check mark independent of font coverage
            const int cx=c.rect.x+c.rect.w-20,cy=c.rect.y+25;
            for(int i=0;i<5;++i) settingsBox(px,{cx+i,cy+i,2,2},0);
            for(int i=0;i<9;++i) settingsBox(px,{cx+4+i,cy+4-i,2,2},0);
        }
    }
    settingsFooter(px,footer,150,447,light);
}
// Firmware controls use the same chrome and leave the clock patch clear.
inline constexpr ReadoutRect UPDATE_MENU{345,29,430,50};
inline constexpr ReadoutRect HOST_INSTALL{594,112,180,52};
inline constexpr ReadoutRect GITHUB_INSTALL{594,209,180,52};
inline constexpr ReadoutRect GITHUB_CHECK{150,311,300,52};
inline constexpr ReadoutRect UPDATE_BACK{594,384,180,56};

inline bool inUpdateControl(ReadoutRect r, int x, int y) {
    return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
}

enum class UpdateHit { None, HostInstall, GithubInstall, Check, Later, Cancel };

inline UpdateHit hitUpdates(int x, int y, bool host, bool github, bool busy,
                            bool waiting = false) {
    if (inUpdateControl(UPDATE_BACK, x, y)) return waiting ? UpdateHit::Cancel : UpdateHit::Later;
    if (busy) return UpdateHit::None;
    if (host && inUpdateControl(HOST_INSTALL, x, y)) return UpdateHit::HostInstall;
    if (github && inUpdateControl(GITHUB_INSTALL, x, y)) return UpdateHit::GithubInstall;
    if (inUpdateControl(GITHUB_CHECK, x, y)) return UpdateHit::Check;
    return UpdateHit::None;
}

inline void updateButton(SettingsSurface pixels, ReadoutRect rect, const char* label, bool enabled = true) {
    const auto fill = enabled ? rgb565(255,119,0) : rgb565(58,58,74);
    const auto ink = enabled ? 0 : rgb565(153,150,186);
    settingsBox(pixels, rect, fill, rect.h / 2);
    const int x = rect.x + (rect.w - textWidth(ClockTime, label)) / 2;
    pixels.text(ClockTime, label, x, rect.y + rect.h / 2 + 8, ink);
}

inline void updateTitle(char (&label)[48], const char* version) {
    snprintf(label, sizeof label, "Update %s available", version);
}

inline void drawUpdatePage(SettingsSurface pixels, const char* host, const char* github,
                           const char* status, bool busy, bool waiting = false) {
    const auto gold = rgb565(255,170,0);
    const auto light = rgb565(153,150,186);
    drawSettingsChrome(pixels, false);
    pixels.text(StatusFont, "FIRMWARE UPDATES", 150, 63, gold);
    pixels.text(ArtDate, host[0] ? "APPROVED BY HOST" : "HOST",
             150, 118, light);
    char label[48];
    if (host[0]) updateTitle(label, host);
    pixels.text(ClockTime, host[0] ? label : "No approved update",
             150, 150, gold);
    if (host[0]) updateButton(pixels, HOST_INSTALL, "Install", !busy);

    pixels.text(ArtDate, "GITHUB", 150, 215, light);
    if (github[0]) updateTitle(label, github);
    pixels.text(ClockTime, github[0] ? label : "No update offered",
             150, 247, gold);
    if (github[0]) updateButton(pixels, GITHUB_INSTALL, "Install", !busy);

    updateButton(pixels, GITHUB_CHECK, "Check for updates", !busy);
    pixels.text(ArtDate, status, 150, 399, light);
    updateButton(pixels, UPDATE_BACK, waiting ? "Cancel" : (host[0] || github[0] ? "Later" : "Back"));
}

} // namespace lcd
