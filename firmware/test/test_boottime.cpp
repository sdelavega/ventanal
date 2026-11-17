// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
#define VENTANAL_STALLTEST 1
#define VENTANAL_BOOTTIMETEST 1
#ifndef VENTANAL_BOOT_CONFIRM_MS
#define VENTANAL_BOOT_CONFIRM_MS 0
#endif
#define main production_test_main
#include "test_wake.cpp"
#undef main
int main() {
    Preferences prefs; assert(prefs.begin("ventanal",false));
    State state; files["/art.vnf"]={1}; files["/weather.vnf"]={1};
    const unsigned count=VENTANAL_BOOT_CONFIRM_MS ? 20 : 32;
    unsigned sleeps=0;
    for(unsigned i=0;i<=count;++i) {
        fakeClock=0;
        const auto before=display.refreshes;
        try { stallRun(prefs,state,75,false); }
        catch(const Sleeping&) { ++sleeps; assert(timerUs==10000000); }
        assert(display.refreshes==before+(i?1:0));
        if(i) {
            unsigned gate=VENTANAL_BOOT_CONFIRM_MS ? VENTANAL_BOOT_CONFIRM_MS : 4000+1500*((i-1)/8);
            assert(fakeClock>=gate && fakeClock<gate+100);
            assert(blitted==(i%2?"/art.vnf":"/weather.vnf"));
        }
    }
    assert(sleeps==count && watchdogFeeds==count);
    // Reset after attempt 12 without its post-attempt sleep: sleep first,
    // then resume at 13. No attempt may run on the watchdog recovery boot.
    uint8_t attempted=12, no=0;
    prefs.putBytes("stall-i",&attempted,1); prefs.putBytes("stall-sleep",&no,1);
    auto before=display.refreshes;
    try { stallRun(prefs,state,75,false); assert(false); } catch(const Sleeping&) {}
    assert(display.refreshes==before);
    fakeClock=0;
    try { stallRun(prefs,state,75,false); } catch(const Sleeping&) {}
    assert(display.refreshes==before+1 && blitted=="/art.vnf");
    armWakeWatchdog(); assert(watchdogTimeout==60000);
    // The first timer boot has attempted=0 but a pending sleep marker.
    // It must use the cached diagnostic route, without normal networking.
    uint32_t run=VENTANAL_STALLTEST; prefs.putBytes("stall-run",&run,sizeof run);
    attempted=0; uint8_t yes=1;
    prefs.putBytes("stall-i",&attempted,1); prefs.putBytes("stall-sleep",&yes,1);
    const int priorConnections=connections;
    fakeClock=0; fakeReset=ESP_RST_DEEPSLEEP; fakeWake=ESP_SLEEP_WAKEUP_TIMER;
    try { setup(); assert(false); } catch(const Sleeping&) {}
    assert(connections==priorConnections);
    assert(fakeClock<(VENTANAL_BOOT_CONFIRM_MS ? VENTANAL_BOOT_CONFIRM_MS : 4000)+100U);
    puts("boottime: all levels, alternating frames, sleep before every attempt, durable watchdog resume and gate passed");
}
