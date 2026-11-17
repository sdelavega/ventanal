// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
#pragma once

#ifdef VENTANAL_LCD_DIAG_HW
#include "board.h"
#include "ch422g.h"
#include "log.h"
#include <cstring>

namespace lcd {
// Diagnostic-only, single-threaded bus access after normal GT911 bring-up.
// Never change CH422G direction: EXIO1/2/3 must remain outputs.
inline bool hwRtcRead(uint8_t reg, uint8_t* data, size_t count) {
    Wire.beginTransmission(RTC_I2C_ADDRESS);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0) return false;
    if (Wire.requestFrom(uint8_t(RTC_I2C_ADDRESS), count) != count) return false;
    for (size_t i = 0; i < count; ++i) data[i] = Wire.read();
    return true;
}
inline bool hwRtcWrite(uint8_t reg, const uint8_t* data, size_t count) {
    Wire.beginTransmission(RTC_I2C_ADDRESS);
    Wire.write(reg);
    Wire.write(data, count);
    return Wire.endTransmission() == 0;
}

inline void hwProbeBegin() {
    log("hw power=USB cell=absent duration_s=60\n");
    log("hw adc=unavailable gpio=-1 divider=none; no sense candidate\n");
    log("hw power_source_detectable=0 backlight_pwm=0 backlight_exio=2\n");
    // Read, freeze, rewrite the SAME calendar, verify exactly, restore control.
    // No arbitrary date, NTP, alarm, offset or battery-backed RAM mutation.
    uint8_t control = 0, saved[7] = {}, readback[7] = {};
    if (!hwRtcRead(0, &control, 1)) {
        log("hw rtc roundtrip=FAIL stage=control-read\n");
        return;
    }
    const uint8_t stopped = control | 0x20;
    const bool stopOk = hwRtcWrite(0, &stopped, 1);
    const bool savedOk = stopOk && hwRtcRead(4, saved, sizeof saved);
    const bool writeOk = savedOk && hwRtcWrite(4, saved, sizeof saved);
    const bool readOk = writeOk && hwRtcRead(4, readback, sizeof readback);
    // Always attempt control restoration, including after an I2C failure.
    const bool restoreOk = hwRtcWrite(0, &control, 1);
    log("hw rtc roundtrip=%s stop=%d read=%d write=%d verify=%d restore=%d ctrl=0x%02x os=%d\n",
        readOk && !memcmp(saved, readback, sizeof saved) && restoreOk
            ? "PASS" : "FAIL",
        stopOk, savedOk, writeOk,
        readOk && !memcmp(saved, readback, sizeof saved), restoreOk,
        control, savedOk ? int(bool(saved[0] & 0x80)) : -1);
}

inline void hwProbeTick(uint32_t now, const Ch422g& expander) {
    static uint32_t started = now;
    static uint32_t lastSecond = UINT32_MAX;
    const uint32_t second = (now - started) / 1000;
    if (second == lastSecond || second > 60) return;
    lastSecond = second;
    uint8_t calendar[7] = {}, control = 0;
    const bool rtcOk = hwRtcRead(0, &control, 1)
                    && hwRtcRead(4, calendar, sizeof calendar);
    // A direct read of 0x26 does not change output mode or the output shadow.
    const bool inputOk = Wire.requestFrom(Ch422g::ADDR_RD_IO, size_t(1)) == 1;
    const int inputs = inputOk ? Wire.read() : 0;
    log("hw t=%lu rtc_ok=%d ctrl=%02x raw=%02x:%02x:%02x date=%02x-%02x-%02x weekday=%02x os=%d ch422g_ok=%d inputs=%02x outputs=%02x\n",
        static_cast<unsigned long>(second), rtcOk, control,
        calendar[2], calendar[1], calendar[0], calendar[6], calendar[5],
        calendar[3], calendar[4], bool(calendar[0] & 0x80), inputOk,
        inputs, expander.outputs());
    if (second == 60) log("hw complete duration_s=60\n");
}
}  // namespace lcd
#endif
