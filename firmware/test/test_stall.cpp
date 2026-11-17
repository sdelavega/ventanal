// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
#define VENTANAL_STALLTEST 1
#ifndef VENTANAL_PWRENTEST
#define VENTANAL_PWRENTEST 0
#endif
#include <driver/gpio.h>
#define main production_test_main
#include "test_wake.cpp"
#undef main
int main() {
    Preferences prefs; assert(prefs.begin("ventanal",false));
    State state; files["/art.vnf"]={1}; files["/weather.vnf"]={1};
    unsigned total=0, sleeps=0;
    for(unsigned chunk=0;chunk<(VENTANAL_PWRENTEST ? 21U : 9U);++chunk) {
        unsigned before=display.refreshes;
        try { stallRun(prefs,state,75,false); }
        catch(const Sleeping&) {
            ++sleeps;
            assert(timerUs==10000000);
            assert(!VENTANAL_PWRENTEST || (pwrHeld && deepHold && pwrOutput && pwrLevel==0));
        }
        total+=display.refreshes-before;
    }
    assert(watchdogFeeds==40 && sleeps==(VENTANAL_PWRENTEST ? 20U : 8U));
    assert(total==40 && readouts==40 && blitted=="/weather.vnf");
    unsigned before=display.refreshes;
    stallRun(prefs,state,75,false); assert(display.refreshes==before);
    // Emulate reset after durable attempt 12: resume at 13, not 12 or 1.
    uint8_t attempted=12; prefs.putBytes("stall-i",&attempted,1);
    try { stallRun(prefs,state,75,false); } catch(const Sleeping&) {}
    assert(display.refreshes-before==(VENTANAL_PWRENTEST ? 1U : 2U));
    assert(blitted==(VENTANAL_PWRENTEST ? "/art.vnf" : "/weather.vnf"));
    assert(watchdogTimeout==0); armWakeWatchdog(); assert(watchdogTimeout==60000);
    // Real setup path must bypass a fourth watchdog strike in diagnostics.
    uint32_t run=VENTANAL_STALLTEST; prefs.putBytes("stall-run",&run,sizeof run);
    attempted=39; prefs.putBytes("stall-i",&attempted,1);
    uint8_t strikes=3; prefs.putBytes("wdt-count",&strikes,1);
    fakeReset=ESP_RST_TASK_WDT; fakeWake=0;
    before=display.refreshes;
    try { setup(); assert(false); } catch(const Sleeping&) {}
    assert(display.refreshes==before+1 && watchdogResets==0);
    assert(timerUs==3600000000ULL);
    puts("stall diagnostic: 40 alternating readouts, timer boundaries, durable resume, completion and 60s watchdog passed");
}
