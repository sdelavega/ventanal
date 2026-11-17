// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
#pragma once
#include <Preferences.h>
// Separate from State: quarantine and owner State migrations preserve this key.
inline int batteryFullMv=0;
inline int manifestBatteryFullMv=0;
inline bool validBatteryFull(int mv) { return mv>=4000 && mv<=4300; }
inline float correctBattery(float volts) {
    return validBatteryFull(batteryFullMv) ? volts*4200.0f/batteryFullMv : volts;
}
inline void loadBattery(Preferences& prefs) {
    int stored=0;
    batteryFullMv=0;
    if(prefs.getBytes("battery-full",&stored,sizeof stored)==sizeof stored && validBatteryFull(stored)) batteryFullMv=stored;
}
inline bool storeBattery(Preferences& prefs,int mv) {
    if(mv!=0 && !validBatteryFull(mv)) return false;
    if(mv!=batteryFullMv && prefs.putBytes("battery-full",&mv,sizeof mv)!=sizeof mv) return false;
    batteryFullMv=mv;
    return true;
}
