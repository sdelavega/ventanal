// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
#include "../lcd43b/power_session.h"
#include "../lcd43b/overlay.h"
#include <cassert>
#include <cstdio>
using namespace lcd;
int main() {
    PowerInputs p{true,true,BatteryLevel::Critical,900000,5,false};
    assert(decidePower(p)==PowerAction::StayOn);
    p.quiet=true; assert(decidePower(p)==PowerAction::BacklightOff);
    p.usbKnown=false; p.level=BatteryLevel::None; p.quiet=false;
    assert(decidePower(p)==PowerAction::Sleep);
    p.timeoutMin=0; assert(decidePower(p)==PowerAction::StayOn);
    p.onUsb=false; p.usbKnown=true; p.level=BatteryLevel::Ok; p.timeoutMin=5;
    p.idleMs=299999; assert(decidePower(p)==PowerAction::StayOn);
    p.idleMs=300000; assert(decidePower(p)==PowerAction::Sleep);
    p.level=BatteryLevel::Critical; p.timeoutMin=0;
    assert(decidePower(p)==PowerAction::ChargeMe);
    p.level=BatteryLevel::None; assert(decidePower(p)==PowerAction::StayOn);
    p.quiet=true; assert(decidePower(p)==PowerAction::BacklightOff);
    assert(pollIntervalMs(true,BatteryLevel::None)==600000);
    assert(pollIntervalMs(false,BatteryLevel::Ok)==1800000);
    assert(pollIntervalMs(true,BatteryLevel::Low)==2400000);
    assert(pollIntervalMs(false,BatteryLevel::Low)==7200000);
    assert(pollIntervalMs(false,BatteryLevel::Critical)==0);
    assert(pollIntervalMs(true,BatteryLevel::Critical)==0);
    assert(pollWaitMs(100,600000)==599900);
    assert(pollWaitMs(600001,600000)==1);
    assert(pollWaitMs(500,0)==0);
    assert(batteryColour(BatteryLevel::Low)==rgb565(221,68,68));
    assert(batteryColour(BatteryLevel::Critical)==rgb565(221,68,68));
    PowerSession s; s.activity(UINT32_MAX-100);
    assert(s.idle(99)==200);
    s.sleep(); assert(!s.screenOn);
    s.wakeTouch(500); assert(s.screenOn && s.idle(500)==0);
    s.sleep(); s.wakeTimer(); assert(!s.screenOn);
    puts("lcd43b power: USB/unknown, timeout, absent/low/critical, poll deadlines, wake passed");
}
