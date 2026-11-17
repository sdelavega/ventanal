// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
#include "../ventanal/battery.h"
#include "../ventanal/logic.h"
#include <cassert>
#include <cmath>
int main() {
    Preferences prefs; prefs.begin("ventanal",false);
    loadBattery(prefs); assert(batteryFullMv==0);
    assert(correctBattery(4.144f)==4.144f);
    assert(storeBattery(prefs,4144));
    assert(std::abs(correctBattery(4.144f)-4.2f)<0.00001f);
    assert(batteryPercent(correctBattery(4.144f))==100);
    assert(isCritical(3.39f) && !isCritical(correctBattery(3.39f)));
    assert(isLow(batteryPercent(3.55f)) && !isLow(batteryPercent(correctBattery(3.55f))));
    batteryFullMv=0; loadBattery(prefs); assert(batteryFullMv==4144);
    const int value=batteryFullMv;
    assert(!storeBattery(prefs,3999) && !storeBattery(prefs,4301));
    assert(batteryFullMv==value);
    // State quarantine/replacement never touches the calibration key.
    prefs.putBytes("state13", "bad", 3); prefs.putBytes("state-bad", "bad",3);
    loadBattery(prefs); assert(batteryFullMv==4144);
    assert(storeBattery(prefs,0)); loadBattery(prefs); assert(batteryFullMv==0);
    int bad=3999; prefs.putBytes("battery-full",&bad,sizeof bad);
    loadBattery(prefs); assert(batteryFullMv==0);
    assert(storeBattery(prefs,4000) && storeBattery(prefs,4300));
}
