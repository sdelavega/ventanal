// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
#include "logic.h"

#include <cstdlib>
#include <cmath>

int batteryPercent(float volts) {
    static constexpr float voltage[] = {
        4.20f, 4.10f, 4.00f, 3.92f, 3.85f, 3.80f,
        3.75f, 3.70f, 3.65f, 3.55f, 3.45f, 3.40f
    };
    static constexpr int percent[] = {100, 90, 80, 70, 60, 50, 40, 30, 20, 10, 5, 0};
    if (volts >= voltage[0]) return 100;
    if (volts <= voltage[11]) return 0;
    for (int i = 1; i < 12; ++i) {
        if (volts >= voltage[i]) {
            const float fraction = (volts - voltage[i]) / (voltage[i - 1] - voltage[i]);
            const float pct = percent[i] + fraction * (percent[i - 1] - percent[i]);
            return static_cast<int>(std::round(pct / 5.0f)) * 5;
        }
    }
    return 0;
}

float smoothVolts(float history[4], uint8_t& count, float sample) {
    if (count < 4) {
        history[count++] = sample;
    } else {
        for (int i = 0; i < 3; ++i) history[i] = history[i + 1];
        history[3] = sample;
        count = 4;
    }
    float sum = 0;
    for (uint8_t i = 0; i < count; ++i) sum += history[i];
    return sum / count;
}

bool isCritical(float volts) { return volts < 3.40f; }
bool isLow(int pct) { return pct <= 10; }

bool inQuiet(int minuteOfDay, int quietStart, int quietEnd) {
    if (quietStart < quietEnd) {
        return minuteOfDay >= quietStart && minuteOfDay < quietEnd;
    }
    if (quietStart > quietEnd) {
        return minuteOfDay >= quietStart || minuteOfDay < quietEnd;
    }
    return false;
}

uint32_t secondsUntilNextWake(time_t now, int wakeMinutes, int quietStart,
                              int quietEnd, bool low) {
    if (wakeMinutes <= 0) return 60;
    const int64_t interval = static_cast<int64_t>(wakeMinutes) * (low ? 4 : 1);
    tm local{};
    if (localtime_r(&now, &local) == nullptr) return 60;
    time_t wake = static_cast<time_t>(-1);
    // Consider both occurrences of repeated local times and reject nonexistent
    // spring-forward boundaries. Each day's boundaries are measured from 00:00.
    for (int day = 0; day < 2; ++day) {
        tm date = local;
        date.tm_mday += day;
        date.tm_hour = 12;
        date.tm_min = date.tm_sec = 0;
        date.tm_isdst = -1;
        if (mktime(&date) == static_cast<time_t>(-1)) continue;
        for (int64_t minute = 0; minute < 1440; minute += interval) {
            for (int dst = 0; dst <= 1; ++dst) {
                tm candidate = date;
                candidate.tm_hour = static_cast<int>(minute / 60);
                candidate.tm_min = static_cast<int>(minute % 60);
                candidate.tm_isdst = dst;
                const time_t value = mktime(&candidate);
                if (value == static_cast<time_t>(-1) || value <= now) continue;
                if (candidate.tm_year != date.tm_year || candidate.tm_mon != date.tm_mon ||
                    candidate.tm_mday != date.tm_mday ||
                    candidate.tm_hour != minute / 60 || candidate.tm_min != minute % 60) continue;
                if (wake == static_cast<time_t>(-1) || value < wake) wake = value;
            }
        }
    }
    if (wake == static_cast<time_t>(-1)) return 60;
    tm target{};
    if (localtime_r(&wake, &target) == nullptr) return 60;
    const int targetMinute = target.tm_hour * 60 + target.tm_min;
    if (inQuiet(targetMinute, quietStart, quietEnd)) {
        if (targetMinute >= quietEnd) ++target.tm_mday;
        target.tm_hour = quietEnd / 60;
        target.tm_min = quietEnd % 60;
        target.tm_sec = 0;
        // Recompute DST at quiet end; choose its earliest future occurrence.
        const time_t boundary = wake;
        wake = static_cast<time_t>(-1);
        for (int dst = -1; dst <= 1; ++dst) {
            tm end = target;
            end.tm_isdst = dst;
            const time_t value = mktime(&end);
            if (value < boundary || value == static_cast<time_t>(-1)) continue;
            if (end.tm_hour != quietEnd / 60 || end.tm_min != quietEnd % 60) continue;
            if (wake == static_cast<time_t>(-1) || value < wake) wake = value;
        }
        // A quiet end in a skipped hour normalizes forward through mktime.
        if (wake == static_cast<time_t>(-1)) {
            target.tm_isdst = -1;
            wake = mktime(&target);
        }
    }
    if (wake == static_cast<time_t>(-1)) return 60;
    const double seconds = difftime(wake, now);
    return seconds < 60 ? 60 : static_cast<uint32_t>(seconds);
}

Screen screenAfterWake(WakeCause c, Screen active, bool solarCached) {
    if (c == WakeCause::Button) {
        if(active==Screen::Art) return Screen::Weather;
        return active==Screen::Weather && solarCached ? Screen::Solar : Screen::Art;
    }
    return active;
}

bool needsRedraw(WakeCause c, Screen show, const Drawn& d,
                 const char* artId, const char* weatherId, int pct) {
    if (c == WakeCause::Button || c == WakeCause::ColdBoot || !d.any) return true;
    if (show == Screen::Weather) return std::strncmp(d.weatherId, weatherId, 17) != 0;
    return std::strncmp(d.artId, artId, 17) != 0 || std::abs(pct - d.pct) >= 5;
}

bool validFrameHeader(const uint8_t* hdr16, size_t fileSize) {
    static constexpr uint8_t expected[16] = {
        'V', 'N', 'F', '1', 0x40, 0x06, 0xb0, 0x04, 0, 0, 0, 0, 0, 0, 0, 0
    };
    return hdr16 != nullptr && fileSize == 960016 &&
           std::memcmp(hdr16, expected, sizeof(expected)) == 0;
}

WakeCause wakeAfterReset(WakeCause wake, bool watchdog) {
    return watchdog ? WakeCause::ColdBoot : wake;
}
bool watchdogLimit(bool watchdog, uint8_t& count) {
    if (watchdog && count < 255) ++count;
    return watchdog && count >= 3;
}
void completeWake(uint8_t& count) { count = 0; }

bool ponStalled(uint32_t elapsedMs, bool seenHigh) {
    return !seenHigh && elapsedMs >= 3000;
}
bool ponRetryBoot(bool softwareReset, uint8_t attempts) {
    return softwareReset && attempts >= 1 && attempts <= 2;
}
bool nextPonRetry(uint8_t& attempts) {
    if(attempts >= 2) return false;
    ++attempts;
    return true;
}
void completePonRetry(uint8_t& attempts) { attempts=0; }
