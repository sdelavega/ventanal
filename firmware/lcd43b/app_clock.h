// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
//
// Clock rendering, NTP and RTC synchronisation.
//
// Part of lcd43b.ino's single translation unit: included only by lcd43b.ino,
// inside its anonymous namespace, in the order listed there. Earlier parts
// provide the declarations later parts use. Do not include it anywhere else.
#pragma once
time_t drawnMinute = -1;
std::atomic<bool> ntpRtcPending{false};
void onNtpSync(timeval*) { ntpRtcPending.store(true); }

bool renderClock(uint16_t* pixels) {
    tm local{}; const time_t now=time(nullptr); localtime_r(&now,&local);
    char timeText[16], dateText[24];
    const bool valid=lcd::formatClock(local,state.manifest.clock24h,timeText,sizeof timeText,dateText,sizeof dateText);
    const int mv=settingsBatteryMv();
    const int percent=lcd::batteryPercent(mv,settings.batteryFullMv);
    lcd::renderReadout(pixels,pixels,settingsPage.active || lcd::isLcarsSlot(clockSlot),
                       timeText,dateText,percent,powerLevel());
    lcd::log("readout valid=%d time=%s date=%s h24=%d battery=none\n",
             valid,timeText,dateText,state.manifest.clock24h);
    drawnMinute=now/60;
    return valid;
}
void seedRtc() {
    if constexpr(lcd::RTC_PCF85063) {
        tm utc{};
        if(lcd::readRtc(Wire,utc)) {
            timeval value{lcd::rtcEpoch(utc),0}; settimeofday(&value,nullptr);
            lcd::log("rtc seed=valid utc=1\n");
        } else lcd::log("rtc seed=skipped invalid-or-OS\n");
    }
}
void saveRtcAfterNtp() {
    if constexpr(lcd::RTC_PCF85063) {
        const time_t now=time(nullptr); tm utc{}; gmtime_r(&now,&utc);
        const bool written=lcd::writeRtc(Wire,utc);
        tm check{}; const bool verified=written && lcd::readRtc(Wire,check)
                            && std::abs(double(lcd::rtcEpoch(check)-now))<=1;
        lcd::log("rtc ntp-write=%d verified=%d OS-clear=%d\n",written,verified,verified);
    }
}
