// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega

// Device manifest parsing: id/url/size/sha256 validation, optional solar,
// up to three spares, the quiet block and quiet-hours arithmetic.

#include "../lcd43b/manifest.h"
#include <cassert>
#include <cstdio>
#include <cstring>

namespace {

const char WEATHER_SHA[] = "2222222222222222222222222222222222222222222222222222222222222222";
const char SPARE_SHA[] = "4444444444444444444444444444444444444444444444444444444444444444";

const char VALID[] = R"({
  "version": 1,
  "screens": {
    "art": {"id": "aaaaaaaaaaaaaaaa", "url": "/devices/lcd43b/frames/art-aaaaaaaaaaaaaaaa.vnr",
            "size": 768016, "sha256": "1111111111111111111111111111111111111111111111111111111111111111"},
    "weather": {"id": "bbbbbbbbbbbbbbbb", "url": "/devices/lcd43b/frames/weather-bbbbbbbbbbbbbbbb.vnr",
                "size": 768016, "sha256": "2222222222222222222222222222222222222222222222222222222222222222"},
    "solar": {"id": "cccccccccccccccc", "url": "/devices/lcd43b/frames/solar-cccccccccccccccc.vnr",
              "size": 768016, "sha256": "3333333333333333333333333333333333333333333333333333333333333333"}
  },
  "spares": [
    {"id": "dddddddddddddddd", "url": "/devices/lcd43b/frames/spare0-dddddddddddddddd.vnr",
     "size": 768016, "sha256": "4444444444444444444444444444444444444444444444444444444444444444"}
  ],
  "quiet": {"start": "23:00", "end": "06:00"}
})";

}  // namespace

int main() {
    {   // A complete manifest parses with every entry copied exactly.
        lcd::DeviceManifest parsed;
        assert(lcd::parseDeviceManifest(VALID, parsed));
        assert(parsed.screens[0].present && parsed.screens[1].present);
        assert(parsed.screens[2].present);
        assert(!strcmp(parsed.screens[0].id, "aaaaaaaaaaaaaaaa"));
        assert(!strcmp(parsed.screens[0].url,
                       "/devices/lcd43b/frames/art-aaaaaaaaaaaaaaaa.vnr"));
        assert(!strcmp(parsed.screens[1].sha256, WEATHER_SHA));
        assert(!strcmp(parsed.screens[2].id, "cccccccccccccccc"));
        assert(parsed.spareCount == 1);
        assert(parsed.spares[0].present);
        assert(!strcmp(parsed.spares[0].sha256, SPARE_SHA));
        assert(parsed.quietValid() && parsed.quietStart == 1380 && parsed.quietEnd == 360);
    }
    {   // Minimal manifest: no solar, spares or quiet block.
        const char minimal[] = R"({"version": 1, "screens": {
            "art": {"id": "aaaaaaaaaaaaaaaa", "url": "/devices/lcd43b/frames/a.vnr", "size": 768016, "sha256": "1111111111111111111111111111111111111111111111111111111111111111"},
            "weather": {"id": "bbbbbbbbbbbbbbbb", "url": "/devices/lcd43b/frames/b.vnr", "size": 768016, "sha256": "2222222222222222222222222222222222222222222222222222222222222222"}}})";
        lcd::DeviceManifest parsed;
        assert(lcd::parseDeviceManifest(minimal, parsed));
        assert(!parsed.screens[2].present && parsed.spareCount == 0);
        assert(!parsed.quietValid());
        lcd::DeviceManifest untouched = parsed;
        assert(!lcd::parseDeviceManifest("{", parsed));
        assert(parsed.spareCount == untouched.spareCount);   // out untouched
    }
    {   // Corrupt manifests are rejected whole.
        lcd::DeviceManifest parsed;
        assert(!lcd::parseDeviceManifest("{}", parsed));
        assert(!lcd::parseDeviceManifest(
            R"({"version": 2, "screens": {}})", parsed));
        assert(!lcd::parseDeviceManifest(
            R"({"version": 1, "screens": {"art": {"id": "aaaaaaaaaaaaaaaa",
              "url": "/devices/lcd43b/frames/a.vnr", "size": 768015,
              "sha256": "1111111111111111111111111111111111111111111111111111111111111111"}}})",
            parsed));   // missing weather
        assert(!lcd::parseDeviceManifest(
            R"({"version": 1, "screens": {
              "art": {"id": "AAAAAAAAAAAAAAAA", "url": "/devices/lcd43b/frames/a.vnr", "size": 768016, "sha256": "1111111111111111111111111111111111111111111111111111111111111111"},
              "weather": {"id": "bbbbbbbbbbbbbbbb", "url": "/devices/lcd43b/frames/b.vnr", "size": 768016, "sha256": "2222222222222222222222222222222222222222222222222222222222222222"}}})",
            parsed));   // uppercase id
        assert(!lcd::parseDeviceManifest(
            R"({"version": 1, "screens": {
              "art": {"id": "aaaaaaaaaaaaaaaa", "url": "/frames/a.vnf", "size": 768016, "sha256": "1111111111111111111111111111111111111111111111111111111111111111"},
              "weather": {"id": "bbbbbbbbbbbbbbbb", "url": "/devices/lcd43b/frames/b.vnr", "size": 768016, "sha256": "2222222222222222222222222222222222222222222222222222222222222222"}}})",
            parsed));   // url outside /devices/
        assert(!lcd::parseDeviceManifest(
            R"({"version": 1, "screens": {
              "art": {"id": "aaaaaaaaaaaaaaaa", "url": "/devices/lcd43b/../../etc/a.vnr", "size": 768016, "sha256": "1111111111111111111111111111111111111111111111111111111111111111"},
              "weather": {"id": "bbbbbbbbbbbbbbbb", "url": "/devices/lcd43b/frames/b.vnr", "size": 768016, "sha256": "2222222222222222222222222222222222222222222222222222222222222222"}}})",
            parsed));   // traversal
        assert(!lcd::parseDeviceManifest(
            R"({"version": 1, "screens": {
              "art": {"id": "aaaaaaaaaaaaaaaa", "url": "/devices/lcd43b/frames/a.vnr", "size": 768016, "sha256": "1111111111111111111111111111111111111111111111111111111111111111"},
              "weather": {"id": "bbbbbbbbbbbbbbbb", "url": "/devices/lcd43b/frames/b.vnr", "size": 768016, "sha256": "2222222222222222222222222222222222222222222222222222222222222222"}},
              "quiet": {"start": "24:00", "end": "06:00"}})",
            parsed));   // impossible clock
    }
    {   // Quiet-hours arithmetic, including the midnight wrap.
        assert(lcd::inQuietMinutes(1410, 1380, 360));  // 23:30 in 23:00-06:00
        assert(lcd::inQuietMinutes(180, 1380, 360));   // 03:00 wrapped in
        assert(!lcd::inQuietMinutes(1320, 1380, 360));  // 22:00 is outside
        assert(!lcd::inQuietMinutes(720, 1380, 360));
        assert(!lcd::inQuietMinutes(720, 720, 720));   // equal disables
        assert(lcd::inQuietMinutes(720, 360, 1380));   // noon in 06:00-23:00
        assert(!lcd::inQuietMinutes(300, 360, 1380));  // before 06:00
        assert(!lcd::inQuietMinutes(1380, 360, 1380));  // 23:00 is outside
        assert(lcd::parseClock("23:00") == 1380);
        assert(lcd::parseClock("00:00") == 0);
        assert(lcd::parseClock("06:60") == -1);
        assert(lcd::parseClock("0600") == -1);
        assert(lcd::parseClock("") == -1);
    }
    {   // Quiet hours apply only once the wall clock is trustworthy: before
        // the first NTP sync (year 1970) the backlight stays on regardless
        // of the persisted window — defined, safe behaviour for a
        // USB-powered display (pinned again by test_lcd43b_persist).
        lcd::DeviceManifest manifest;
        manifest.quietStart = 1380;
        manifest.quietEnd = 360;
        assert(!lcd::quietApplies(1970, 1410, manifest));   // unsynced clock
        assert(!lcd::quietApplies(2022, 1410, manifest));  // still implausible
        assert(lcd::quietApplies(2026, 1410, manifest));   // 23:30, in window
        assert(!lcd::quietApplies(2026, 720, manifest));   // noon, outside
        manifest.quietStart = -1;                           // no window stored
        assert(!lcd::quietApplies(2026, 1410, manifest));
    }
    puts("lcd43b manifest: schema, spares, quiet hours, pre-sync, rejection"
         " passed");
    return 0;
}