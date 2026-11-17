// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
#pragma once

#include <ctime>
#include <cstdint>
#include <cstring>

enum class Screen : uint8_t { Art = 0, Weather = 1, Solar = 2, Extra0 = 3, Extra1 = 4 };
enum class WakeCause : uint8_t { ColdBoot, Timer, Button };

int batteryPercent(float volts);
// Initialize count to zero; history holds the oldest through newest samples.
float smoothVolts(float history[4], uint8_t& count, float sample);
bool isCritical(float volts);
bool isLow(int pct);
bool inQuiet(int minuteOfDay, int quietStart, int quietEnd);
// Caller configures TZ and calls tzset(); equal quiet endpoints disable quiet.
uint32_t secondsUntilNextWake(time_t now, int wakeMinutes, int quietStart,
                              int quietEnd, bool low);
Screen screenAfterWake(WakeCause c, Screen active, bool solarCached=false);

struct Drawn {
    char artId[17];
    char weatherId[17];
    int8_t pct;
    bool any;
};

bool needsRedraw(WakeCause c, Screen show, const Drawn& d,
                 const char* artId, const char* weatherId, int pct);
bool validFrameHeader(const uint8_t* hdr16, size_t fileSize);

struct ReadoutRect {
    int16_t x, y, w, h;
    bool lcars;
};

WakeCause wakeAfterReset(WakeCause wake, bool watchdog);
bool watchdogLimit(bool watchdog, uint8_t& count);
void completeWake(uint8_t& count);

// The PON wait must release BUSYN before the long DRF wait begins.
// seenHigh includes only samples in the inclusive 500..3000 ms PON window.
bool ponStalled(uint32_t elapsedMs, bool seenHigh);
bool ponRetryBoot(bool softwareReset, uint8_t attempts);
bool nextPonRetry(uint8_t& attempts);
void completePonRetry(uint8_t& attempts);
