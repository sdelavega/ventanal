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
    const char* expected[]={"/art.vnf","/weather.vnf","/solar.vnf","/extra0.vnf","/extra1.vnf","/spare0.vnf","/spare1.vnf","/spare2.vnf"};
    for(int round=0;round<4;++round) {
        // Reset MCU clock each wake; NVS cursor must survive this.
        fakeClock=0; downloadPaths.clear();
        wake(round?ESP_SLEEP_WAKEUP_TIMER:0);
        assert(downloadPaths.size()==2);
        assert(downloadPaths[0]==expected[2*round] && downloadPaths[1]==expected[2*round+1]);
        assert(fakeClock<=155000 && watchdogFeeds==0);
        fakeReset=ESP_RST_DEEPSLEEP;
    }
    assert(canDownloadSpare(104999) && !canDownloadSpare(105000));
    // Always-changing core art must not starve deferred solar/extras/spares.
    files.clear(); preferences.clear();
    for(int round=0;round<4;++round) {
        fakeClock=0; downloadPaths.clear();
        memset(art.id,'a'+round,16);
        wake(ESP_SLEEP_WAKEUP_TIMER);
        assert(downloadPaths[0]==expected[2*round]);
    }
    // Failed transfers also advance fairly and never commit an incoming ID.
    files.clear(); preferences.clear(); frameOK=false;
    for(int round=0;round<4;++round) {
        fakeClock=0; downloadPaths.clear();
        wake(ESP_SLEEP_WAKEUP_TIMER);
        assert(downloadPaths.size()==2 && downloadPaths[0]==expected[2*round]);
    }
    frameOK=true;
    // A charge-me PON stall shuts down immediately with all wake sources disabled.
    State state{}; rememberPanel(state,0,false,time(nullptr),2);
    displayHook=[] { fakeBusy=0; fakeClock=panelStarted+3000; timerCallback(nullptr); };
    assert(timerUs>0 && buttonArmed);
    try { refreshPanel(); assert(false); } catch(const Sleeping&) {}
    assert(timerUs==0 && !buttonArmed && pwrHeld && pwrLevel==0);
    assert(pendingPanel.magic==0 && pendingPanel.attempts==0 && !timerRunning);
    puts("budget: 8 screens, 30s transfers, durable priority, changing-art fairness, no watchdog feeds, charge-me limit passed");
}
