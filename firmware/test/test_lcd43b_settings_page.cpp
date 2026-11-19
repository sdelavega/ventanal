// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
#include "../lcd43b/settings_page.h"
#include "../lcd43b/local_scene.h"
#include "../lcd43b/wifi_retry.h"
#include <cassert>
#include <vector>
#include <memory>
#include <cstdio>

static void call_sign_status_stays_between_controls() {
    using namespace lcd;
    constexpr int first = 342;
    constexpr int rows = 32;
    const auto settings = defaultSettings(false);
    std::vector<uint16_t> plain(WIDTH * rows), actual(WIDTH * rows);
    drawSettingsPage({plain.data(), first, rows}, settings, true, true, "", CallState::None);
    drawSettingsPage({actual.data(), first, rows}, settings, true, true, "", CallState::Verified);
    assert(actual == plain);

    const auto check = [&](CallState state, const char* label) {
        assert(textWidth(ArtDate, label) < 310 - 150);
        auto expected = plain;
        SettingsSurface(expected.data(), first, rows).text(ArtDate, label, 150, 359, rgb565(153,150,186));
        drawSettingsPage({actual.data(), first, rows}, settings, true, true, "", state);
        assert(actual == expected && actual != plain);
    };
    check(CallState::Unverified, "CALL SIGN UNVERIFIED");
    check(CallState::Rejected, "CALL SIGN NOT FOUND");
}

int main(int argc,char** argv) {
    using namespace lcd;
    call_sign_status_stays_between_controls();
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
    assert(hitSettings(160,380,true,false)==SettingsHit::None);
    assert(hitSettings(400,380,true,false)==SettingsHit::SetupWifi);
    assert(hitSettings(-1,100,true,true)==SettingsHit::None);
    assert(inUpdateControl(UPDATE_MENU, 500, 55));
    assert(hitUpdates(640, 130, true, false, false) == UpdateHit::HostInstall);
    assert(hitUpdates(640, 130, true, false, true) == UpdateHit::None);
    assert(hitUpdates(640, 235, true, false, false) == UpdateHit::None);
    assert(hitUpdates(640, 235, true, true, false) == UpdateHit::GithubInstall);
    assert(hitUpdates(300, 335, false, false, false) == UpdateHit::Check);
    assert(hitUpdates(660, 410, true, true, true) == UpdateHit::Later);
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
    for(int y=374;y<426;++y) for(int x=150;x<360;++x) assert(px[1+y*WIDTH+x]==0);
    drawSettingsPage(px.data()+1,s,true,true,"VENTANAL · TEST · OFFLINE · v1.0.0");
    assert(px[1+400*WIDTH+352]!=0); // generic calibration control is drawn
    drawUpdatePage(px.data()+1, "65535.65535.65535", "2.0.0", "Checked within the last week", false);
    assert(px.front()==0x1234 && px.back()==0x1234);
    assert(hitUpdates(660, 410, true, true, true, true) == UpdateHit::Cancel);
    assert(textWidth(ClockTime, "Update 65535.65535.65535 available") < UPDATE_MENU.w - 10);
    assert(textWidth(ClockTime, "Update 65535.65535.65535 available") < HOST_INSTALL.x - 150);
    for (bool updates : {false, true}) {
        constexpr int bandRows = 40;
        const auto paint = [&](SettingsSurface band) {
            if (updates) drawUpdatePage(band, "65535.65535.65535", "2.0.0",
                                        "Waiting for refresh...", true, true);
            else drawSettingsPage(band, s, true, true,
                                  "VENTANAL · SYNTHETIC-DESK · WI-FI OFFLINE · v65535.65535.65535");
        };
        paint(px.data()+1);
        auto scene = std::make_unique<LocalPage>();
        auto patched = std::make_unique<LocalPage>();
        std::vector<uint16_t> band(WIDTH * bandRows);
        for (int y = 0; y < HEIGHT; y += bandRows) {
            paint({band.data(), y, bandRows});
            assert(scene->encodeBand(band.data(), y, bandRows));
            assert(patched->copyBand(*scene, y, bandRows));
        }
        for (int y = 0; y < HEIGHT; y += 10) {
            patched->decode(band.data(), y, 10);
            assert(std::equal(band.begin(), band.begin() + 10 * WIDTH, px.begin() + 1 + y * WIDTH));
        }
    }
    {
        constexpr int bandRows = 40;
        std::vector<uint16_t> band(WIDTH * bandRows);
        auto old = std::make_unique<LocalPage>();
        auto next = std::make_unique<LocalPage>();
        drawSettingsPage(px.data() + 1, s, true, true, "VENTANAL");
        assert(old->encode(px.data() + 1));
        s.brightness = 75;
        const auto damage = settingsDamage(SettingsHit::Brightness75);
        for (int y = 0; y < HEIGHT; y += bandRows) {
            if (y >= damage.y + damage.h || y + bandRows <= damage.y) {
                assert(next->copyBand(*old, y, bandRows));
            } else {
                drawSettingsPage({band.data(), y, bandRows}, s, true, true, "VENTANAL");
                assert(next->encodeBand(band.data(), y, bandRows));
            }
        }
        drawSettingsPage(px.data() + 1, s, true, true, "VENTANAL");
        for (int y = 0; y < HEIGHT; y += bandRows) {
            next->decode(band.data(), y, bandRows);
            assert(std::equal(band.begin(), band.end(), px.begin() + 1 + y * WIDTH));
        }
    }
    LocalScene<4, 1, 2> bounded{};
    const uint16_t compressible[]{0, 0, 0, 0};
    const uint16_t incompressible[]{1, 2, 3, 4};
    assert(bounded.encode(compressible));
    assert(!bounded.encode(incompressible));
    assert(settingsDamage(SettingsHit::TimeoutNever).y == 186);
    assert(settingsDamage(SettingsHit::None).h == 0);

    WifiRetry retry;
    uint32_t at = UINT32_MAX - 1000;
    assert(!retry.waiting() && !retry.due(at));
    for (const uint32_t wait : {15000, 30000, 60000, 120000, 240000, 240000}) {
        retry.finished(at, false);
        assert(retry.waiting() && !retry.due(at + wait - 1) && retry.due(at + wait));
        at += wait;
    }
    retry.finished(at, true);
    assert(!retry.waiting() && !retry.due(at));

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
        uint16_t region[CLOCK_GAP.w*CLOCK_GAP.h];
        renderReadout(region,nullptr,true,"2:32 PM","TUE 17 NOV",-1,BatteryLevel::None);
        pasteReadout(px.data()+1,region,CLOCK_GAP);
        FILE* f=fopen(argv[1],"wb"); assert(f);
        assert(fwrite(px.data()+1,2,WIDTH*HEIGHT,f)==size_t(WIDTH)*HEIGHT); fclose(f);
    }
    puts("lcd43b settings page: 384000 chrome pixels equal server fixture");
    puts("lcd43b settings page: controls, guards, idle, drawing bounds passed");
}
