// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
#include "../lcd43b/screens.h"
#include <cassert>
#include <cstdio>
int main() {
    using namespace lcd;
    for(int solar=0;solar<=1;++solar) for(int spares=0;spares<=3;++spares) {
        DeviceSettings s;
        int slots[6]; const int n=activeSlots(solar,spares,s,slots,6);
        assert(n==2+solar);
        for(int i=0;i<n;++i) assert(screenSlot(i,solar,spares,s)==i);
        assert(screenSlot(-1,solar,spares,s)==-1 && screenSlot(n,solar,spares,s)==-1);
        assert(activeSlots(solar,spares,s,slots,0)==0);
        assert(activeSlots(solar,spares,s,slots,1)==1 && slots[0]==0);
        for(int i=0;i<n;++i) {
            int index=0,cursor=2;
            remapSelection(index,cursor,i,solar,spares,s);
            assert(index==i && cursor==2);
        }
    }
    auto s=defaultSettings(false); s.showArt=false;
    int slots[6];
    assert(activeSlots(true,3,s,slots,6)==2 && slots[0]==1 && slots[1]==2);
    s.showWeather=false;
    assert(activeSlots(false,3,s,slots,6)==1 && slots[0]==1);
    int index=0,cursor=-1;
    s.showWeather=true;
    remapSelection(index,cursor,0,false,3,s);
    assert(screenSlot(index,false,3,s)==1 && cursor==-1);
    s.showArt=true;
    remapSelection(index,cursor,4,false,3,s);
    assert(screenSlot(index,false,3,s)==1);
    puts("lcd43b screens: art/weather/solar rotation, visibility, fallback passed");
}
