// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega

// LCD overlays: RGB565 helpers, the OFFLINE mark and the waiting screen,
// with pixel clipping proven safe at panel edges.

#include "../lcd43b/overlay.h"
#include <cassert>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {

constexpr int PIXELS = lcd::WIDTH * lcd::HEIGHT;   // small buffer per row usage
uint16_t screen[PIXELS];

int countColour(uint16_t colour) {
    int total = 0;
    for (int i = 0; i < PIXELS; ++i)
        if (screen[i] == colour) ++total;
    return total;
}

}  // namespace

int main() {
    { // Synthetic nibble coverage pins RGB565 blending, clipping and glyph bounds.
        const uint8_t bits[]={0x08,0xf0};
        const lcd::AAGlyph glyphs[]={{0,3,1,3,0,-1}};
        const lcd::AAFont font{bits,glyphs,65,65,2};
        uint16_t px[10]; std::fill(px,px+10,0x001f);
        lcd::drawTextAA(px,5,2,1,1,"A",font,0xf800);
        assert(px[0]==0x001f && px[1]==0x001f && px[2]==0x880e && px[3]==0xf800);
        for(int i=4;i<10;++i) assert(px[i]==0x001f);
        lcd::drawTextAA(px,5,2,-2,1,"A",font,0xffff);
        assert(px[0]==0xffff);
    }

    {
        uint16_t glyph[64*32]={};
        lcd::drawTextAA(glyph,64,32,4,24,"2:32",ClockTime,0xffff);
        bool edge=false,solid=false;
        for(auto c:glyph) { edge|=c!=0 && c!=0xffff; solid|=c==0xffff; }
        assert(edge && solid); // generated font really contains partial coverage
    }
    assert(lcd::rgb565(255, 0, 0) == 0xF800);
    assert(lcd::rgb565(0, 255, 0) == 0x07E0);
    assert(lcd::rgb565(0, 0, 255) == 0x001F);
    assert(lcd::rgb565(255, 255, 255) == 0xFFFF);
    {   // The OFFLINE mark paints red status pixels on a black panel.
        memset(screen, 0, sizeof screen);
        lcd::drawOfflineMark(screen, lcd::WIDTH, lcd::HEIGHT);
        assert(countColour(lcd::RGB565_RED) > 0);
        assert(countColour(lcd::RGB565_WHITE) == 0);
    }
    {   // The waiting screen clears to black with white host information.
        memset(screen, 0xFF, sizeof screen);
        lcd::drawWaiting(screen, lcd::WIDTH, lcd::HEIGHT,
                         "http://192.168.0.10:18780");
        assert(countColour(lcd::RGB565_BLACK) > 0);
        assert(countColour(lcd::RGB565_WHITE) > 0);
        int drawnUrlPixels = 0;
        for (int y = 96; y < 112; ++y)
            for (int x = 12; x < 500; ++x)
                if (screen[y * lcd::WIDTH + x] == lcd::RGB565_WHITE) ++drawnUrlPixels;
        assert(drawnUrlPixels > 0);   // the URL line exists below the title
    }
    {   // Text clips instead of writing out of bounds at either edge.
        memset(screen, 0, sizeof screen);
        lcd::drawText(screen, lcd::WIDTH, lcd::HEIGHT, WaitingBody,
                      "EDGE", lcd::WIDTH - 8, 240, lcd::RGB565_WHITE);
        lcd::drawText(screen, lcd::WIDTH, lcd::HEIGHT, WaitingBody,
                      "EDGE", -50, -40, lcd::RGB565_WHITE);
        assert(countColour(lcd::RGB565_WHITE) > 0);   // partial glyphs drew
        // No writes escaped the buffer: row 240 stays black past the text.
        int escape = 0;
        for (int y = 0; y < 16; ++y)
            for (int x = 0; x < 24; ++x)
                if (screen[(236 + y) * lcd::WIDTH + x] == lcd::RGB565_WHITE) ++escape;
        assert(escape == 0);
    }
    {   // Refresh feedback label state machine (host-pinned): a refresh
        // gesture shows REFRESHING immediately; completion shows UP TO DATE
        // for ~2 s when nothing changed; a failed fetch shows OFFLINE
        // styling; an unfinished frame clears REFRESHING quietly. A
        // 24-hour outage keeps the failure badge until content arrives.
        lcd::RefreshLabelState label;
        ventanal::ContentStaleness content{};
        constexpr uint32_t now = ventanal::kValidEpoch;
        auto current = [&] { return label.label(now, content); };
        assert(current() == lcd::RefreshLabel::None);
        assert(lcd::refreshLabelText(current()) == nullptr);
        label.begin(1000);                    // gesture: immediate feedback
        assert(current() == lcd::RefreshLabel::Refreshing);
        assert(label.label(0, content) == lcd::RefreshLabel::Refreshing);
        assert(!label.tick(2500));           // no hold timeout while refreshing
        label.finish(3000, false, false, false);   // fetch ok, nothing new
        assert(current() == lcd::RefreshLabel::UpToDate);
        assert(!label.tick(4500));           // hold not expired yet
        assert(label.tick(5100));            // ~2 s hold expired
        assert(current() == lcd::RefreshLabel::None);
        label.begin(7000);
        label.finish(8000, true, false, false);    // fetch failed
        assert(current() == lcd::RefreshLabel::Offline);
        assert(label.tick(10100));
        label.begin(11000);
        label.finish(11500, false, true, false);   // new content: redraw only
        assert(current() == lcd::RefreshLabel::None);
        assert(!label.tick(20000));
        label.begin(21000);
        label.finish(21500, false, false, true);    // required frame lost
        assert(current() == lcd::RefreshLabel::None);
        assert(!label.tick(23600));
        content.observe(now, true);
        const auto overdue = [&] { return label.label(now + ventanal::kContentTimeout, content); };
        assert(overdue() == lcd::RefreshLabel::Failed);
        assert(!strcmp(lcd::refreshLabelText(overdue()), "DOWNLOAD FAILED"));
        assert(!label.tick(24000));
        assert(overdue() == lcd::RefreshLabel::Failed);
        content.clearOverdue();
        assert(overdue() == lcd::RefreshLabel::None);
        label.begin(24000);
        label.finish(24500, true, false, true);
        assert(current() == lcd::RefreshLabel::Offline);
        label.clear();
        label.begin(25000);
        label.clear();                        // a scheduled redraw replaces it
        assert(current() == lcd::RefreshLabel::None);
    }
    {   // The label renders in the bottom-left overlay corner, in the same
        // font/style as the OFFLINE mark, and clips safely at panel edges.
        memset(screen, 0, sizeof screen);
        lcd::drawRefreshLabel(screen, lcd::WIDTH, lcd::HEIGHT, "REFRESHING...",
                              lcd::RGB565_WHITE);
        assert(countColour(lcd::RGB565_WHITE) > 0);   // legible text pixels
        int outside = 0;
        for (int y = 0; y < lcd::HEIGHT - 50; ++y)      // nothing above the corner
            for (int x = 0; x < lcd::WIDTH; ++x)
                if (screen[y * lcd::WIDTH + x] == lcd::RGB565_WHITE) ++outside;
        for (int y = lcd::HEIGHT - 50; y < lcd::HEIGHT; ++y)
            for (int x = 260; x < lcd::WIDTH; ++x)     // nothing right of the badge
                if (screen[y * lcd::WIDTH + x] == lcd::RGB565_WHITE) ++outside;
        assert(outside == 0);
        // Red styling for the offline and download-failed labels.
        memset(screen, 0, sizeof screen);
        lcd::drawRefreshLabel(screen, lcd::WIDTH, lcd::HEIGHT,
                              lcd::refreshLabelText(lcd::RefreshLabel::Offline),
                              lcd::RGB565_RED);
        assert(countColour(lcd::RGB565_RED) > 0);
        memset(screen, 0, sizeof screen);
        lcd::drawRefreshLabel(screen, lcd::WIDTH, lcd::HEIGHT,
                              lcd::refreshLabelText(lcd::RefreshLabel::Failed),
                              lcd::RGB565_RED);
        assert(countColour(lcd::RGB565_RED) > 0);
        // Every label text fits the panel width at the overlay size.
        for (const char* text : {"REFRESHING...", "UP TO DATE", "OFFLINE",
                                 "DOWNLOAD FAILED"})
            assert(lcd::textWidth(StatusFont, text) < lcd::WIDTH - 32);
    }
    for (bool lcars : {false,true}) {
        std::fill(screen,screen+PIXELS,0xffff);
        uint16_t region[250*80];
        const auto rect=lcars ? lcd::CLOCK_GAP : lcd::ART_CORNER;
        uint16_t art[250*80]; std::fill(art,art+250*80,0xffff);
        lcd::renderReadout(region,art,lcars,"2:32 PM","TUE 17 NOV",-1,lcd::BatteryLevel::None);
        lcd::pasteReadout(screen,region,rect);
        int changed=0;
        for(int y=0;y<lcd::HEIGHT;++y) for(int x=0;x<lcd::WIDTH;++x) {
            bool inside=lcars ? (x>=548 && x<752 && y<26) : (x>=550 && y<80);
            if(screen[y*lcd::WIDTH+x]!=0xffff) { assert(inside); ++changed; }
        }
        assert(changed>0);
        uint16_t before[PIXELS]; memcpy(before,screen,sizeof screen);
        // None always omits percentage, regardless of its numeric input.
        lcd::renderReadout(region,art,lcars,"2:32 PM","TUE 17 NOV",82,lcd::BatteryLevel::None);
        lcd::pasteReadout(screen,region,rect);
        assert(!memcmp(before,screen,sizeof screen));
        lcd::renderReadout(region,art,lcars,"2:32 PM","TUE 17 NOV",82,lcd::BatteryLevel::Ok);
        lcd::pasteReadout(screen,region,rect);
        assert(memcmp(before,screen,sizeof screen));
    }
    {
        uint16_t art[250*80], region[250*80];
        std::fill(art,art+250*80,0xffff);
        lcd::renderReadout(region,art,false,"","",-1,lcd::BatteryLevel::None);
        assert(!memcmp(art,region,sizeof art)); // invalid clock leaves art untouched
        lcd::renderReadout(region,nullptr,true,"","",-1,lcd::BatteryLevel::None);
        for(int i=0;i<204*26;++i) assert(region[i]==0); // no invalid date or glyph
        lcd::renderReadout(region,art,false,"2:32 PM","TUE 17 NOV",-1,lcd::BatteryLevel::Ok);
        uint16_t without[250*80]; memcpy(without,region,sizeof without);
        lcd::renderReadout(region,art,false,"2:32 PM","TUE 17 NOV",-1,lcd::BatteryLevel::None);
        assert(!memcmp(without,region,sizeof without));
        assert(region[0]==0xffff && region[79*250+249]==0xffff); // feather reaches clear edges
        assert(region[249]!=0xffff); // shade exists at top right
        // Exact glyph layout pins grouped time/date across both clock formats.
        for(const char* time : {"2:32 PM","12:59 PM","14:32"}) {
            uint16_t expected[204*26] = {};
            const int dateX=6+lcd::textWidth(ClockTime,time)+10;
            lcd::drawText(expected,204,26,ClockTime,time,6,20,lcd::rgb565(255,170,0));
            lcd::drawText(expected,204,26,ClockDate,"WED 30 NOV",dateX,18,lcd::rgb565(153,150,186));
            lcd::renderReadout(region,nullptr,true,time,"WED 30 NOV",-1,lcd::BatteryLevel::None);
            assert(!memcmp(expected,region,sizeof expected));
            assert(dateX+lcd::textWidth(ClockDate,"WED 30 NOV")+7
                   <204-6-lcd::textWidth(ClockDate,"100%"));
        }
    }
    {
        const auto rect = lcd::readoutRect(1, 1);
        uint16_t fresh[250 * 80]{}, stale[250 * 80]{}, expected[250 * 80]{};
        lcd::renderReadout(fresh, nullptr, true, "12:59 PM", "WED 30 NOV", 100,
                           lcd::BatteryLevel::Ok);
        lcd::renderReadout(stale, nullptr, true, "12:59 PM", "WED 30 NOV", 100,
                           lcd::BatteryLevel::Ok, 1);
        const int dateX = 6 + lcd::textWidth(ClockTime, "12:59 PM") + 10;
        lcd::drawText(expected, rect.w, rect.h, ClockDate, "WEATHER 1 H AGO",
                      dateX, 39, lcd::rgb565(153,150,186));
        int agePixels = 0;
        for (int i = 0; i < rect.w * rect.h; ++i) {
            if (expected[i]) {
                assert(i / rect.w >= lcd::CLOCK_GAP.h);
                assert(fresh[i] == 0);
                assert(stale[i] == expected[i]);
                ++agePixels;
            } else {
                assert(stale[i] == fresh[i]);
            }
        }
        assert(agePixels > 0);
        lcd::renderReadout(stale, nullptr, true, "12:59 PM", "WED 30 NOV", 100,
                           lcd::BatteryLevel::Ok);
        const auto freshRect = lcd::readoutRect(1);
        assert(freshRect.h < rect.h);
        std::fill(screen, screen + PIXELS, 0);
        lcd::pasteReadout(screen, stale, freshRect);
        for (int y = 0; y < rect.h; ++y)
            assert(!memcmp(screen + y * lcd::WIDTH + rect.x, fresh + y * rect.w,
                           rect.w * sizeof(uint16_t)));
    }
    assert(!lcd::isLcarsSlot(0) && lcd::isLcarsSlot(1) && lcd::isLcarsSlot(2) && !lcd::isLcarsSlot(3));
    puts("lcd43b overlay: RGB565, OFFLINE mark, waiting screen, clipping,"
         " refresh label passed");
    return 0;
}