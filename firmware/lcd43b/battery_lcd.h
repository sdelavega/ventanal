// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
#pragma once
#include <cmath>
namespace lcd {
enum class BatteryLevel { None, Ok, Low, Critical };
// Same voltage curve and 5% rounding as the e-paper model. Calibration is
// device-local: a measured full voltage maps to nominal 4200 mV.
inline int batteryPercent(int millivolts, int fullMv) {
    if(millivolts<=0) return -1;
    const float mv=fullMv>=4000 && fullMv<=4300 ? millivolts*4200.0f/fullMv : millivolts;
    const int voltage[]={4200,4100,4000,3920,3850,3800,3750,3700,3650,3550,3450,3400};
    const int percent[]={100,90,80,70,60,50,40,30,20,10,5,0};
    if(mv>=4200) return 100;
    if(mv<=3400) return 0;
    for(int i=1;i<12;++i) if(mv>=voltage[i])
        return int(std::round((percent[i]+(mv-voltage[i])*(percent[i-1]-percent[i])/(voltage[i-1]-voltage[i]))/5.0f))*5;
    return 0;
}
inline BatteryLevel batteryLevel(int mv, int percent) {
    if(mv<=0 || percent<0) return BatteryLevel::None;
    return mv<3400 ? BatteryLevel::Critical : percent<=10 ? BatteryLevel::Low : BatteryLevel::Ok;
}
} // namespace lcd
