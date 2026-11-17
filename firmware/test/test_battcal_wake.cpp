// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
#define main baseline_main
#include "test_wake.cpp"
#undef main
int main() {
    fakeReset=ESP_RST_POWERON;
    display.voltage=4.144f; batteryCount=0;
    manifestBatteryFullMv=4144;
    wake(0);
    assert(readoutPct==100 && batteryFullMv==4144);
    assert(std::string(manifestBoardQuery).find("bat_mv=4144")!=std::string::npos);
    const auto calibration=preferences["battery-full"];
    // Offline cold boot loads the key without a manifest or RTC battery history.
    online=false; batteryFullMv=0; batteryCount=0;
    wake(0); assert(readoutPct==100 && batteryFullMv==4144);
    // Exercise the actual State quarantine path; calibration is retained.
    online=true; preferences["state13"]={1,2,3}; batteryCount=0;
    wake(0);
    assert((preferences["state-bad"]==std::vector<uint8_t>{1,2,3}));
    assert(preferences["battery-full"]==calibration && batteryFullMv==4144);
    // The actual setup protection uses the corrected voltage on offline wakes.
    online=false; display.voltage=3.39f; batteryCount=0;
    int priorCharges=charges;
    wake(0); assert(charges==priorCharges && timerUs>0);
    Preferences prefs; prefs.begin("ventanal",false); assert(storeBattery(prefs,4300));
    display.voltage=3.45f; batteryCount=0;
    wake(0,true); assert(charges==priorCharges+1);
    std::puts("battery wake: immediate gain, raw telemetry, cold/offline persistence, State quarantine and corrected critical protection passed");
}
