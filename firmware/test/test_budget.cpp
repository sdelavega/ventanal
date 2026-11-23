// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
#define main baseline_main
#include "test_wake.cpp"
#undef main
int main() {
    wifiDelay=17000; // Includes a conservative 2s boot plus 15s Wi-Fi.
    timeDelay=15000; manifestDelay=30000; frameDelay=30000;
    manifestExtras.solar=weather;
    for(int i=0;i<2;++i) { manifestExtras.extraScreens[i]=art; snprintf(manifestExtras.names[i],49,"extra-%d",i); }
    manifestExtras.count=3;
    for(int i=0;i<3;++i) manifestExtras.spares[i]=art;
    displayHook=[] { delay(20000); };
    fakeReset=ESP_RST_POWERON;
    const char* expected[]={"/weather.vnf","/art.vnf","/solar.vnf","/extra0.vnf","/extra1.vnf","/spare0.vnf","/spare1.vnf","/spare2.vnf"};
    // Each wake completes its first frame and gives the leftover 13 s to the
    // next (too short in this model). An unfinished visible screen is retried
    // before the next one; spares follow, rotating by their own cursor.
    const std::vector<std::vector<int>> rounds{{0,1},{1,2},{2,3},{3,4},{4,5},{6,7},{5,7},{7}};
    for(size_t round=0;round<rounds.size();++round) {
        // Reset MCU clock each wake; NVS cursor must survive this.
        fakeClock=0; downloadPaths.clear();
        wake(round?ESP_SLEEP_WAKEUP_TIMER:0);
        assert(downloadPaths.size()==rounds[round].size());
        for(size_t i=0;i<downloadPaths.size();++i) assert(downloadPaths[i]==expected[rounds[round][i]]);
        assert(fakeClock<=126000 && watchdogFeeds==0);
        fakeReset=ESP_RST_DEEPSLEEP;
    }
    for(const char* path : expected) assert(files.count(path));
    assert(frameWakeRemainingMs(104999)==1 && frameWakeRemainingMs(105000)==0);
    // Always-changing core art is the first visible screen after weather;
    // the next visible screen still gets every wake's leftover budget (real
    // downloads resume, so that leftover is progress, not a lost attempt).
    files.clear(); preferences.clear();
    for(int round=0;round<4;++round) {
        fakeClock=0; downloadPaths.clear();
        memset(art.id,'a'+round,16);
        wake(ESP_SLEEP_WAKEUP_TIMER);
        assert(downloadPaths.size()==2 && downloadPaths[0]==(round ? expected[1] : expected[0]));
        assert(downloadPaths[1]==(round ? expected[2] : expected[1]));
    }
    // Unfinished weather and art keep their priority over the other screens.
    files.clear(); preferences.clear(); frameOK=false;
    for(int round=0;round<4;++round) {
        fakeClock=0; downloadPaths.clear();
        wake(ESP_SLEEP_WAKEUP_TIMER);
        assert(downloadPaths.size()==2 && downloadPaths[0]==expected[0] && downloadPaths[1]==expected[1]);
    }
    frameOK=true;
    // A charge-me PON stall shuts down immediately with all wake sources disabled.
    State state{}; rememberPanel(state,0,false,time(nullptr),2);
    displayHook=[] { fakeBusy=0; fakeClock=panelStarted+3000; timerCallback(nullptr); };
    assert(timerUs>0 && buttonArmed);
    try { refreshPanel(); assert(false); } catch(const Sleeping&) {}
    assert(timerUs==0 && !buttonArmed && pwrHeld && pwrLevel==0);
    assert(pendingPanel.magic==0 && pendingPanel.attempts==0 && !timerRunning);
    puts("budget: 8 screens, shared deadline, durable rotation, no watchdog feeds, charge-me limit passed");
}
