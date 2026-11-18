// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega

// Persisted cross-reboot state: validated quiet settings, the published
// screen shape (solar/spares), cache ids and the selection. Pinned here:
// an offline boot during quiet hours keeps the backlight on until the
// clock becomes trustworthy, then applies the persisted window.

#include "../lcd43b/persist.h"
#include "../lcd43b/clock.h"

#include "Preferences.h"

#include <cassert>
#include <cstdio>
#include <cstring>

namespace {

// The fake Preferences store is a global; start every scenario empty.
void freshNvs() { preferences.clear(); }

}  // namespace

int main() {
    {   // Validated quiet settings persist and reload on a fresh boot.
        freshNvs();
        Preferences prefs;
        assert(prefs.begin("ventanal", false));
        lcd::DeviceManifest published;
        published.quietStart = 23 * 60;   // parsed+validated by the manifest
        published.quietEnd = 6 * 60;      // parser, so persisting is safe
        lcd::saveQuiet(prefs, published);
        lcd::DeviceManifest boot;         // fresh boot: defaults
        assert(!boot.quietValid());
        assert(lcd::loadQuiet(prefs, boot));
        assert(boot.quietValid());
        assert(boot.quietStart == 1380 && boot.quietEnd == 360);
    }
    {   // A manifest without quiet clears any previously stored window.
        freshNvs();
        Preferences prefs;
        assert(prefs.begin("ventanal", false));
        lcd::DeviceManifest published;
        published.quietStart = 1380;
        published.quietEnd = 360;
        lcd::saveQuiet(prefs, published);
        lcd::DeviceManifest without;
        lcd::saveQuiet(prefs, without);   // quietValid() == false
        lcd::DeviceManifest boot;
        assert(!lcd::loadQuiet(prefs, boot));
        assert(!boot.quietValid());
    }
    {   // Offline boot during quiet hours: the server is unreachable, so
        // the window comes from NVS. Before the first time sync the year is
        // implausible and the backlight stays on (defined behaviour); once
        // the clock is valid the window applies; a touch still overrides.
        freshNvs();
        Preferences prefs;
        assert(prefs.begin("ventanal", false));
        lcd::DeviceManifest published;
        published.quietStart = 1380;
        published.quietEnd = 360;
        lcd::saveQuiet(prefs, published);
        lcd::DeviceManifest boot;
        assert(lcd::loadQuiet(prefs, boot));
        assert(!lcd::quietApplies(1970, 1410, boot));   // 23:30, unsynced: on
        assert(lcd::quietApplies(2026, 1410, boot));    // 23:30, synced: off
        assert(lcd::quietApplies(2026, 180, boot));     // 03:00 wrapped: off
        assert(!lcd::quietApplies(2026, 720, boot));   // noon: on
    }
    {   // The published shape (solar, spare count) persists so the boot
        // screen list matches the cache before the first manifest fetch.
        freshNvs();
        Preferences prefs;
        assert(prefs.begin("ventanal", false));
        lcd::DeviceManifest published;
        published.screens[2].present = true;
        published.spareCount = 3;
        lcd::savePublishShape(prefs, published);
        lcd::DeviceManifest boot;
        assert(!boot.screens[2].present && boot.spareCount == 0);
        lcd::loadPublishShape(prefs, boot);
        assert(boot.screens[2].present && boot.spareCount == 3);
        int slots[6]; assert(lcd::activeSlots(true,3,lcd::DeviceSettings{},slots,6)==3);   // selection can normalise
    }
    {   // Offline reboot: only NVS and the RTC time are available, no fetch.
        freshNvs();
        Preferences writer;
        assert(writer.begin("ventanal", false));
        lcd::DeviceManifest published;
        published.clock24h = false;
        lcd::savePublishShape(writer, published);
        writer.end();
        Preferences reader;
        assert(reader.begin("ventanal", true));
        lcd::DeviceManifest boot;
        assert(boot.clock24h);
        lcd::loadPublishShape(reader, boot);
        assert(!boot.clock24h);
        tm local{};
        local.tm_year=126; local.tm_mon=10; local.tm_mday=17;
        local.tm_wday=2; local.tm_hour=14; local.tm_min=32;
        char time[16], date[24];
        assert(lcd::formatClock(local,boot.clock24h,time,sizeof time,date,sizeof date));
        assert(!strcmp(time,"2:32 PM"));
        reader.end();
        // A subsequent server change back to 24 h also survives reboot.
        assert(writer.begin("ventanal", false));
        published.clock24h = true;
        lcd::savePublishShape(writer, published);
        writer.end();
        assert(reader.begin("ventanal", true));
        lcd::loadPublishShape(reader, boot);
        assert(boot.clock24h);
        assert(lcd::formatClock(local,boot.clock24h,time,sizeof time,date,sizeof date));
        assert(!strcmp(time,"14:32"));
    }
    {   // Cache ids and the selection round-trip under the phase-1 keys.
        freshNvs();
        Preferences prefs;
        assert(prefs.begin("ventanal", false));
        char ids[lcd::SCREEN_SLOTS][17] = {};
        strlcpy(ids[0], "aaaaaaaaaaaaaaaa", sizeof ids[0]);
        strlcpy(ids[5], "0123456789abcdef", sizeof ids[5]);
        for (int slot = 0; slot < lcd::SCREEN_SLOTS; ++slot)
            lcd::saveCacheId(prefs, slot, ids[slot]);
        char loaded[lcd::SCREEN_SLOTS][17] = {};
        assert(lcd::loadCacheIds(prefs, loaded));
        assert(!strcmp(loaded[0], "aaaaaaaaaaaaaaaa"));
        assert(!strcmp(loaded[1], ""));
        assert(!strcmp(loaded[5], "0123456789abcdef"));
        lcd::saveSelection(prefs, 4, 2);
        int screenIndex = 0, spareCursor = 0;
        assert(lcd::loadSelection(prefs, screenIndex, spareCursor));
        assert(screenIndex == 4 && spareCursor == 2);
    }
    {   // Nothing stored yet: defaults, no crash.
        freshNvs();
        Preferences prefs;
        assert(prefs.begin("ventanal", false));
        char ids[lcd::SCREEN_SLOTS][17] = {};
        assert(!lcd::loadCacheIds(prefs, ids));
        assert(!strcmp(ids[0], ""));
        lcd::DeviceManifest boot;
        assert(!lcd::loadQuiet(prefs, boot));
        assert(!boot.quietValid());
        boot.clock24h = false;
        lcd::loadPublishShape(prefs, boot);
        assert(boot.clock24h); // missing key defaults to 24 h (older firmware)
    }
    {   // Fresh device: selection defaults to art/spare0.
        freshNvs();
        Preferences prefs;
        assert(prefs.begin("ventanal", false));
        int screenIndex = 9, spareCursor = 9;
        lcd::loadSelection(prefs, screenIndex, spareCursor);
        assert(screenIndex == 0 && spareCursor == 0);
    }
    puts("lcd43b persist: quiet, publish shape, cache ids, offline boot in"
         " quiet hours passed");
    return 0;
}
