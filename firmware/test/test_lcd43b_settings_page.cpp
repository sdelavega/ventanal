// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
#include "../lcd43b/settings_page.h"
#include <cassert>
#include <vector>
#include <cstdio>
int main(int argc,char** argv) {
    using namespace lcd;
    for (const auto& c : SETTINGS_CONTROLS) {
        assert(c.rect.h>=52);
        assert(hitSettings(c.rect.x+c.rect.w/2,c.rect.y+c.rect.h/2,true,true)==c.hit);
    }
    for(int y : {108,212,316}) {
        assert(hitSettings(305,y,true,true)==SettingsHit::None);
        assert(hitSettings(785,y,true,true)==SettingsHit::None);
    }
    assert(hitSettings(420,108,true,true)==SettingsHit::None);
    assert(hitSettings(429,108,true,true)==SettingsHit::None);
    assert(hitSettings(310,82,true,true)==SettingsHit::Brightness25);
    assert(hitSettings(419,133,true,true)==SettingsHit::Brightness25);
    assert(hitSettings(310,134,true,true)==SettingsHit::None);
    assert(hitSettings(425,145,true,true)==SettingsHit::None);
    assert(hitSettings(770,316,false,false)==SettingsHit::None);
    assert(hitSettings(400,380,true,false)==SettingsHit::None);
    assert(hitSettings(-1,100,true,true)==SettingsHit::None);
    auto s=defaultSettings(false);
    assert(applySettingsHit(s,SettingsHit::Brightness25,false,-1) && s.brightness==25);
    assert(applySettingsHit(s,SettingsHit::Timeout15,false,-1) && s.timeoutMin==15);
    assert(applySettingsHit(s,SettingsHit::ToggleArt,false,-1));
    assert(!applySettingsHit(s,SettingsHit::ToggleWeather,false,-1) && s.showWeather);
    assert(!applySettingsHit(s,SettingsHit::ToggleSolar,false,-1));
    assert(!applySettingsHit(s,SettingsHit::BatteryFull,false,-1));
    assert(applySettingsHit(s,SettingsHit::BatteryFull,true,4200) && s.batteryFullMv==4200);
    SettingsSession session;
    session.open(100); assert(!session.expired(30099)); assert(session.expired(30100));
    session.touch(30100); assert(!session.expired(60099)); assert(session.expired(60100));
    session.open(UINT32_MAX-100); assert(session.expired(29900));
    std::vector<uint16_t> px(WIDTH*HEIGHT+2,0x1234);
    drawSettingsPage(px.data()+1,s,false,false,"VENTANAL · TEST · WI-FI OK · v1.0.0");
    assert(px.front()==0x1234 && px.back()==0x1234);
    for(int y=0;y<26;++y) for(int x=548;x<752;++x) assert(px[1+y*WIDTH+x]==0);
    for(int y=290;y<342;++y) for(int x=630;x<780;++x) assert(px[1+y*WIDTH+x]==0);
    for(int y=374;y<426;++y) for(int x=150;x<580;++x) assert(px[1+y*WIDTH+x]==0);
    drawSettingsPage(px.data()+1,s,true,true,"VENTANAL · TEST · OFFLINE · v1.0.0");
    assert(px[1+400*WIDTH+360]!=0); // generic calibration control is drawn
    // Independent server draw_lcd_frame fixture, labels omitted consistently.
    drawSettingsChrome(px.data()+1,false);
    FILE* fixture=fopen("fixtures/lcd43b-settings-chrome.rle","rb"); assert(fixture);
    size_t index=0;
    unsigned char run[4];
    while(fread(run,1,4,fixture)==4) {
        const unsigned count=run[0]|(unsigned(run[1])<<8);
        const uint16_t colour=run[2]|(unsigned(run[3])<<8);
        assert(count && index+count<=size_t(WIDTH)*HEIGHT);
        for(unsigned i=0;i<count;++i) assert(px[1+index++]==colour);
    }
    assert(feof(fixture) && index==size_t(WIDTH)*HEIGHT); fclose(fixture);
    assert(px.front()==0x1234 && px.back()==0x1234);
    if(argc==2) {
        auto preview=defaultSettings(false);
        drawSettingsPage(px.data()+1,preview,true,false,"VENTANAL · LCD43B · WI-FI OK · v1.0.0");
        drawTopBarClock(px.data()+1,"2:32 PM","TUE 17 NOV",-1,BatteryLevel::None);
        FILE* f=fopen(argv[1],"wb"); assert(f);
        assert(fwrite(px.data()+1,2,WIDTH*HEIGHT,f)==size_t(WIDTH)*HEIGHT); fclose(f);
    }
    puts("lcd43b settings page: 384000 chrome pixels equal server fixture");
    puts("lcd43b settings page: controls, guards, idle, drawing bounds passed");
}
