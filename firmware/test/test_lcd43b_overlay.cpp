// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega

// LCD overlays: RGB565 helpers, the OFFLINE mark and the waiting screen,
// with pixel clipping proven safe at panel edges.

#include "../lcd43b/overlay.h"
#include <cassert>
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
        lcd::drawText(screen, lcd::WIDTH, lcd::HEIGHT, Inter_Medium12cap7b,
                      "EDGE", lcd::WIDTH - 8, 240, lcd::RGB565_WHITE);
        lcd::drawText(screen, lcd::WIDTH, lcd::HEIGHT, Inter_Medium12cap7b,
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
        // styling; a manifest that arrived but lost a required frame shows
        // DOWNLOAD FAILED (the pass is a failure — UP TO DATE would lie
        // over stale artwork); new content clears the label (the redraw
        // speaks); and the hold expires so the cached frame redraws clean.
        lcd::RefreshLabelState label;
        assert(label.label() == lcd::RefreshLabel::None);
        assert(lcd::refreshLabelText(label.label()) == nullptr);
        label.begin(1000);                    // gesture: immediate feedback
        assert(label.label() == lcd::RefreshLabel::Refreshing);
        assert(!label.tick(2500));           // no hold timeout while refreshing
        label.finish(3000, false, false, false);   // fetch ok, nothing new
        assert(label.label() == lcd::RefreshLabel::UpToDate);
        assert(!label.tick(4500));           // hold not expired yet
        assert(label.tick(5100));            // ~2 s hold expired
        assert(label.label() == lcd::RefreshLabel::None);
        label.begin(7000);
        label.finish(8000, true, false, false);    // fetch failed
        assert(label.label() == lcd::RefreshLabel::Offline);
        assert(label.tick(10100));
        label.begin(11000);
        label.finish(11500, false, true, false);   // new content: redraw only
        assert(label.label() == lcd::RefreshLabel::None);
        assert(!label.tick(20000));
        label.begin(21000);
        label.finish(21500, false, false, true);    // required frame lost
        assert(label.label() == lcd::RefreshLabel::Failed);
        assert(!strcmp(lcd::refreshLabelText(label.label()),
                       "DOWNLOAD FAILED"));
        assert(!label.tick(23000));           // hold not expired yet
        assert(label.tick(23600));            // ~2 s hold expired, redrawn clean
        assert(label.label() == lcd::RefreshLabel::None);
        // Offline outranks a frame failure (the fetch itself never ran).
        label.begin(24000);
        label.finish(24500, true, false, true);
        assert(label.label() == lcd::RefreshLabel::Offline);
        label.clear();
        label.begin(25000);
        label.clear();                        // a scheduled redraw replaces it
        assert(label.label() == lcd::RefreshLabel::None);
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
            assert(lcd::textWidth(Antonio_Regular18pt7b, text) < lcd::WIDTH - 32);
    }
    puts("lcd43b overlay: RGB565, OFFLINE mark, waiting screen, clipping,"
         " refresh label passed");
    return 0;
}