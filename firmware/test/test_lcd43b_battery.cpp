// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
#include "../lcd43b/battery_lcd.h"
#include "../ventanal/logic.h"
#include <cassert>
#include <cstdio>
int main() {
    assert(lcd::batteryPercent(-1,4200)==-1); assert(lcd::batteryPercent(0,4200)==-1);
    const int mv[]={4200,4100,4000,3920,3850,3800,3750,3700,3650,3550,3450,3400};
    const int pct[]={100,90,80,70,60,50,40,30,20,10,5,0};
    // Check the production e-paper curve at every LCD table knot and between
    // knots, so either copy changing independently fails this host test.
    for(int i=0;i<12;++i) {
        assert(lcd::batteryPercent(mv[i],4200)==pct[i]);
        assert(::batteryPercent(mv[i]/1000.0f)==pct[i]);
    }
    for(int millivolts=3401;millivolts<4200;millivolts+=17)
        assert(lcd::batteryPercent(millivolts,4200)==::batteryPercent(millivolts/1000.0f));
    assert(lcd::batteryPercent(4100,4100)==100);
    assert(lcd::batteryPercent(5000,4200)==100 && lcd::batteryPercent(3000,4200)==0);
    assert(lcd::batteryPercent(4200,0)==100);
    assert(lcd::batteryLevel(-1,-1)==lcd::BatteryLevel::None);
    assert(lcd::batteryLevel(3400,0)==lcd::BatteryLevel::Low);
    assert(lcd::batteryLevel(3399,0)==lcd::BatteryLevel::Critical);
    assert(lcd::batteryLevel(3550,10)==lcd::BatteryLevel::Low);
    assert(lcd::batteryLevel(3600,15)==lcd::BatteryLevel::Ok);
    puts("lcd43b battery: curve, calibration, absent, thresholds passed");
}
