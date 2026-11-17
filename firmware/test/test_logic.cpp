// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
#include "../ventanal/logic.h"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cmath>

static time_t localTime(int year, int month, int day, int hour, int minute,
                        int second = 0) {
    tm value{};
    value.tm_year = year - 1900;
    value.tm_mon = month - 1;
    value.tm_mday = day;
    value.tm_hour = hour;
    value.tm_min = minute;
    value.tm_sec = second;
    value.tm_isdst = -1;
    return mktime(&value);
}

static void test_battery() {
    assert(batteryPercent(4.25f) == 100);
    assert(batteryPercent(3.775f) == 45);
    assert(batteryPercent(3.30f) == 0);
    assert(batteryPercent(3.86f) == 60);
    const float volts[] = {4.20f, 4.10f, 4.00f, 3.92f, 3.85f, 3.80f,
                           3.75f, 3.70f, 3.65f, 3.55f, 3.45f, 3.40f};
    const int pct[] = {100, 90, 80, 70, 60, 50, 40, 30, 20, 10, 5, 0};
    for (unsigned i = 0; i < 12; ++i) assert(batteryPercent(volts[i]) == pct[i]);
    assert(isCritical(3.39f));
    assert(!isCritical(3.40f));
    assert(isLow(0) && isLow(10) && !isLow(11));

    float history[4]{};
    uint8_t count = 0;
    assert(smoothVolts(history, count, 1.0f) == 1.0f && count == 1);
    assert(smoothVolts(history, count, 2.0f) == 1.5f && count == 2);
    assert(smoothVolts(history, count, 3.0f) == 2.0f && count == 3);
    assert(smoothVolts(history, count, 4.0f) == 2.5f && count == 4);
    assert(smoothVolts(history, count, 5.0f) == 3.5f && count == 4);
    assert(smoothVolts(history, count, 6.0f) == 4.5f && count == 4);
}

static void test_quiet_and_wake() {
    assert(inQuiet(23 * 60, 1380, 360));
    assert(inQuiet(5 * 60 + 59, 1380, 360));
    assert(!inQuiet(6 * 60, 1380, 360));
    assert(!inQuiet(22 * 60 + 59, 1380, 360));
    assert(inQuiet(600, 600, 720));
    assert(!inQuiet(720, 600, 720));
    assert(!inQuiet(600, 600, 600));
    assert(secondsUntilNextWake(localTime(2026, 11, 15, 14, 10), 30, 1380, 360, false) == 1200);
    assert(secondsUntilNextWake(localTime(2026, 11, 15, 22, 40), 30, 1380, 360, false) == 26400);
    assert(secondsUntilNextWake(localTime(2026, 11, 1, 0, 40), 30, 1380, 360, false) == 22800);
    assert(secondsUntilNextWake(localTime(2026, 3, 8, 0, 40), 30, 1380, 360, false) == 15600);
    assert(secondsUntilNextWake(localTime(2026, 11, 15, 14, 10), 30, 1380, 360, true) == 6600);
    assert(secondsUntilNextWake(localTime(2026, 11, 15, 14, 30), 30, 1380, 360, false) == 1800);
    assert(secondsUntilNextWake(localTime(2026, 11, 15, 14, 29, 59), 30, 1380, 360, false) == 60);
    assert(secondsUntilNextWake(localTime(2026, 11, 15, 10, 10), 30, 600, 720, false) == 6600);
    assert(secondsUntilNextWake(localTime(2026, 11, 15, 23, 50), 30, 0, 0, false) == 600);
    // With quiet hours disabled, the repeated 01:00 is the next boundary.
    tm firstHour{};
    firstHour.tm_year = 2026 - 1900;
    firstHour.tm_mon = 10;
    firstHour.tm_mday = 1;
    firstHour.tm_hour = 1;
    firstHour.tm_min = 40;
    firstHour.tm_isdst = 1;
    assert(secondsUntilNextWake(mktime(&firstHour), 30, 0, 0, false) == 1200);
    assert(secondsUntilNextWake(mktime(&firstHour), 30, 0, 90, false) == 3000);
    assert(secondsUntilNextWake(localTime(2026, 3, 8, 1, 40), 30, 0, 0, false) == 1200);
    assert(secondsUntilNextWake(localTime(2026, 3, 8, 1, 40), 120, 0, 0, false) == 4800);
    assert(secondsUntilNextWake(localTime(2026, 12, 31, 23, 50), 30, 0, 0, false) == 600);
}

static void test_fetch_failed_no_redraw() {
    const Drawn d{"art", "weather", 70, true};
    assert(!needsRedraw(WakeCause::Timer, Screen::Weather, d, "art", "weather", 65));
}

static void test_screens_and_redraw() {
    const Drawn d{"art", "weather", 70, true};
    const Screen screens[] = {Screen::Art, Screen::Weather};
    for (Screen screen : screens) {
        assert(screenAfterWake(WakeCause::Timer, screen) == screen);
        assert(screenAfterWake(WakeCause::ColdBoot, screen) == screen);
        assert(screenAfterWake(WakeCause::Button, screen) != screen);
        assert(needsRedraw(WakeCause::Button, screen, d, "art", "weather", 70));
        assert(needsRedraw(WakeCause::ColdBoot, screen, d, "art", "weather", 70));
    }
    assert(needsRedraw(WakeCause::Timer, Screen::Art, d, "art", "weather", 65));
    assert(needsRedraw(WakeCause::Timer, Screen::Art, d, "art", "weather", 75));
    assert(!needsRedraw(WakeCause::Timer, Screen::Art, d, "art", "weather", 70));
    assert(!needsRedraw(WakeCause::Timer, Screen::Art, d, "art", "changed", 66));
    assert(needsRedraw(WakeCause::Timer, Screen::Art, d, "changed", "weather", 70));
    assert(needsRedraw(WakeCause::Timer, Screen::Weather, d, "art", "changed", 70));
    assert(!needsRedraw(WakeCause::Timer, Screen::Weather, d, "changed", "weather", 70));
    const Drawn empty{};
    assert(needsRedraw(WakeCause::Timer, Screen::Weather, empty, "", "", 0));
}

static void test_valid_frame_header() {
    uint8_t header[16] = {'V', 'N', 'F', '1', 0x40, 0x06, 0xb0, 0x04};
    assert(validFrameHeader(header, 960016));
    assert(!validFrameHeader(header, 960015));
    header[3] = '0';
    assert(!validFrameHeader(header, 960016));
    header[3] = '1';
    header[4] = 0x41;
    assert(!validFrameHeader(header, 960016));
    header[4] = 0x40;
    header[6] = 0xb1;
    assert(!validFrameHeader(header, 960016));
    header[6] = 0xb0;
    header[15] = 1;
    assert(!validFrameHeader(header, 960016));
    assert(!validFrameHeader(nullptr, 960016));
}

int main() {
    uint8_t resets=0;
    assert(wakeAfterReset(WakeCause::Button, false)==WakeCause::Button);
    assert(wakeAfterReset(WakeCause::Button, true)==WakeCause::ColdBoot);
    assert(wakeAfterReset(WakeCause::Timer, true)==WakeCause::ColdBoot);
    assert(!watchdogLimit(true,resets) && resets==1);
    assert(!watchdogLimit(true,resets) && resets==2);
    assert(watchdogLimit(true,resets) && resets==3);
    assert(!watchdogLimit(false,resets) && resets==3); // scheduled retry
    assert(watchdogLimit(true,resets) && resets==4);
    completeWake(resets); assert(resets==0);
    resets=255; assert(watchdogLimit(true,resets) && resets==255);

    assert(setenv("TZ", "America/New_York", 1) == 0);
    tzset();
    test_battery();
    test_quiet_and_wake();
    test_fetch_failed_no_redraw();
    test_screens_and_redraw();
    test_valid_frame_header();
    std::puts("all tests passed");
}
