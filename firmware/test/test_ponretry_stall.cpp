// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
#define VENTANAL_STALLTEST 1
#define VENTANAL_PONRETRYTEST 1
#define main production_test_main
#include "test_wake.cpp"
#undef main
int main() {
    Preferences prefs; assert(prefs.begin("ventanal",false));
    files["/art.vnf"]={1}; files["/weather.vnf"]={1};
    uint32_t run=VENTANAL_STALLTEST;
    prefs.putBytes("stall-run",&run,sizeof run);
    State state{};
    try { stallRun(prefs,state,75,true); assert(false); } catch(const Sleeping&) {}
    assert(display.refreshes==0 && timerUs==10000000);
    for(unsigned i=1;i<=20;++i) {
        fakeReset=ESP_RST_DEEPSLEEP;fakeWake=ESP_SLEEP_WAKEUP_TIMER;
        displayHook=[] { fakeBusy=0;fakeClock=panelStarted+3000;timerCallback(nullptr); };
        try { setup();assert(false); } catch(const Restarting&) {}
        assert(pendingPanel.sample==i && pendingPanel.attempts==1);
        assert(blitted==(i%2?"/art.vnf":"/weather.vnf"));
        const auto before=connections;
        fakeReset=ESP_RST_SW;fakeClock=0;
        displayHook=[] { fakeBusy=1;fakeClock=panelStarted+500;timerCallback(nullptr); };
        try { setup();assert(false); } catch(const Sleeping&) {}
        assert(connections==before && pendingPanel.magic==0);
        assert(timerUs==(i==20?3600000000ULL:10000000ULL));
        assert(watchdogResets==0 && watchdogTimeout==60000);
    }
    assert(display.refreshes==40 && watchdogFeeds==20);
    puts("ponretry diagnostic: 20 actual-sleep boundaries, alternating samples, same-sample retry and no networking passed");
}
