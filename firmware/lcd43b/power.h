// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
#pragma once
#include <cstdint>
#include "battery_lcd.h"
namespace lcd {
struct PowerInputs {
    bool onUsb; bool usbKnown; BatteryLevel level;
    uint32_t idleMs; uint8_t timeoutMin; bool quiet;
};
enum class PowerAction { StayOn, BacklightOff, Sleep, ChargeMe };
inline PowerAction decidePower(const PowerInputs& p) {
    if(p.usbKnown && p.onUsb)
        return p.quiet?PowerAction::BacklightOff:PowerAction::StayOn;
    if(p.level==BatteryLevel::Critical) return PowerAction::ChargeMe;
    if(p.timeoutMin && p.idleMs>=uint32_t(p.timeoutMin)*60000)
        return PowerAction::Sleep;
    return p.quiet?PowerAction::BacklightOff:PowerAction::StayOn;
}
// Zero means no timer (critical battery: touch only).
inline uint32_t pollIntervalMs(bool screenOn,BatteryLevel level) {
    if(level==BatteryLevel::Critical) return 0;
    return (screenOn?600000UL:1800000UL)*(level==BatteryLevel::Low?4:1);
}
inline uint32_t pollWaitMs(uint32_t elapsed,uint32_t interval) {
    return !interval?0:elapsed>=interval?1:interval-elapsed;
}
struct PowerSession {
    bool screenOn=true;
    uint32_t lastActivity=0;
    void activity(uint32_t now) { lastActivity=now; }
    uint32_t idle(uint32_t now) const { return now-lastActivity; }
    void sleep() { screenOn=false; }
    void wakeTouch(uint32_t now) { screenOn=true; activity(now); }
    void wakeTimer() {} // background work must leave the screen dark
};
} // namespace lcd
