// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
#include "../lcd43b/persist.h"
#include <cassert>
#include <cstdio>
int main() {
    assert(lcd::defaultSettings(false).timeoutMin==0);
    assert(lcd::defaultSettings(true).timeoutMin==5);
    Preferences prefs;
    auto s=lcd::defaultSettings(false);
    assert(!lcd::loadSettings(prefs,s) && s.brightness==100);
    s.brightness=25; s.timeoutMin=15; s.showArt=false; s.batteryFullMv=4200;
    lcd::saveSettings(prefs,s);
    lcd::DeviceSettings loaded;
    assert(lcd::loadSettings(prefs,loaded));
    assert(loaded.brightness==25 && loaded.timeoutMin==15 && !loaded.showArt && loaded.batteryFullMv==4200);
    for(size_t i=0;i<preferences["settings"].size();++i) {
        const auto original=preferences["settings"];
        preferences["settings"][i]^=0xff;
        assert(!lcd::loadSettings(prefs,loaded) && loaded.brightness==100 && loaded.timeoutMin==0);
        preferences["settings"]=original;
    }
    preferences["settings"].pop_back();
    assert(!lcd::loadSettings(prefs,loaded));
    auto v=lcd::defaultSettings(false);
    assert(lcd::setVisibility(v,0,false,false));
    assert(!lcd::setVisibility(v,1,false,false)); // unpublished Solar cannot save last visible
    assert(lcd::setVisibility(v,1,false,true));
    assert(!lcd::setVisibility(v,2,false,true));
    assert(!lcd::setVisibility(v,7,true));
    uint16_t src[]={0xffff,0xf800,0x07e0,0x001f,0}, dst[5];
    lcd::dimRgb565(src,dst,5,100);
    for(int i=0;i<5;++i) assert(src[i]==dst[i]);
    lcd::dimRgb565(src,dst,5,50);
    assert(dst[0]==0x7bef && dst[1]==0x7800 && dst[2]==0x03e0 && dst[3]==0x000f);
    lcd::dimRgb565(src,dst,5,25);
    assert(dst[0]==0x39e7 && dst[1]==0x3800 && dst[2]==0x01e0 && dst[3]==0x0007);
    assert(src[0]==0xffff);
    lcd::dimRgb565(src,src,5,50); assert(src[0]==0x7bef);
    puts("lcd43b settings: defaults, versioned NVS corruption, last-visible, RGB565 passed");
}
