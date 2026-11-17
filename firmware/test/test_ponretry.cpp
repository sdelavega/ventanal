// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
#define main production_test_main
#include "test_wake.cpp"
#undef main
int main() {
    assert(!ponStalled(2999,false));
    assert(ponStalled(3000,false));
    assert(!ponStalled(180000,true));
    assert(!ponRetryBoot(false,1));
    assert(!ponRetryBoot(true,0) && !ponRetryBoot(true,3));
    assert(ponRetryBoot(true,1) && ponRetryBoot(true,2));
    uint8_t attempts=0;
    assert(nextPonRetry(attempts) && attempts==1);
    assert(nextPonRetry(attempts) && attempts==2);
    assert(!nextPonRetry(attempts) && attempts==2);
    uint8_t strikes=1;
    assert(!watchdogLimit(false,strikes) && strikes==1);
    completePonRetry(attempts); assert(attempts==0);
    // A stalled offline button draw must survive a simulated software reset.
    fakeReset=ESP_RST_POWERON;
    wake(0);
    const int beforeConnections=connections, beforeManifests=manifests;
    online=false; fakeBusy=0;
    displayHook=[] {
        // Startup/reset HIGH pulses are excluded, including the last ms before 500.
        for(unsigned ms : {100,499}) {
            fakeBusy=1; fakeClock=panelStarted+ms; timerCallback(nullptr);
        }
        assert(!panelSeenHigh);
        fakeBusy=0; fakeClock=panelStarted+2999; timerCallback(nullptr);
        assert(pendingPanel.attempts==0);
        fakeClock=panelStarted+3000; timerCallback(nullptr); };
    fakeWake=ESP_SLEEP_WAKEUP_EXT0;
    try { setup(); assert(false); } catch(const Restarting&) {}
    assert(pendingPanel.magic==ponMagic && pendingPanel.attempts==1);
    assert(panelState().active==Screen::Art);
    const auto timestamp=pendingPanel.drawnAt;
    const auto pct=pendingPanel.pct;
    assert(!pendingPanel.offline); // button path preserves original offline=false
    fakeReset=ESP_RST_SW; fakeClock=0;
    displayHook=[] { fakeBusy=1; fakeClock=panelStarted+500; timerCallback(nullptr);
        fakeBusy=0; fakeClock=panelStarted+25000; timerCallback(nullptr); };
    wake(0);
    assert(connections==beforeConnections && manifests==beforeManifests);
    assert(blitted=="/art.vnf" && pendingPanel.drawnAt==timestamp && pendingPanel.pct==pct);
    assert(pendingPanel.magic==0 && pendingPanel.attempts==0 && !timerRunning);
    assert(watchdogResets==0 && watchdogTimeout==180000);
    // An offline ordinary wake retains the true offline indicator and exact
    // overlay inputs even when time/battery would differ on the retry boot.
    fakeReset=ESP_RST_POWERON;
    displayHook=[] { fakeBusy=0;fakeClock=panelStarted+3000;timerCallback(nullptr); };
    try { setup();assert(false); } catch(const Restarting&) {}
    assert(pendingPanel.offline && offlineDraw);
    const auto offlineTime=readoutTime;
    const int offlinePct=readoutPct;
    fakeReset=ESP_RST_SW;fakeClock=0;
    display.voltage=4.2f;
    const int networkBefore=connections;
    displayHook={};fakeBusy=1;
    wake(0);
    assert(offlineDraw && readoutTime==offlineTime && readoutPct==offlinePct);
    assert(connections==networkBefore && pendingPanel.magic==0);
    // Only HIGH within the inclusive 500..3000 ms window latches.
    for(unsigned ms : {500,1000,3000}) {
        rememberPanel(State{},75,false,123456);
        displayHook=[ms] { fakeBusy=1; fakeClock=panelStarted+ms; timerCallback(nullptr);
            assert(panelSeenHigh);
            fakeBusy=0; fakeClock=panelStarted+180000; timerCallback(nullptr); };
        refreshPanel();
        assert(!timerRunning && pendingPanel.magic==0);
    }
    rememberPanel(State{},75,false,123456);
    displayHook=[] { fakeBusy=1; fakeClock=panelStarted+3001; timerCallback(nullptr); };
    try { refreshPanel(); assert(false); } catch(const Restarting&) {}
    assert(!panelSeenHigh && pendingPanel.attempts==1);
    // Two software restarts, then sleep with panel power low and no third restart.
    State state{}; state.active=Screen::Weather;
    rememberPanel(state,75,true,123456);
    displayHook=[] { fakeBusy=0; fakeClock=panelStarted+3000; timerCallback(nullptr); };
    for(int n=1;n<=2;++n) {
        try { refreshPanel(); assert(false); } catch(const Restarting&) {}
        assert(pendingPanel.attempts==n && pendingPanel.magic==ponMagic);
    }
    try { refreshPanel(); assert(false); } catch(const Sleeping&) {}
    assert(pendingPanel.magic==0 && panelPower==0 && timerUs>10000000);
    // Stale markers on power/deep-sleep/watchdog resets never bypass normal boot.
    for(auto reason : {ESP_RST_POWERON,ESP_RST_DEEPSLEEP,ESP_RST_TASK_WDT}) {
        pendingPanel.magic=ponMagic; pendingPanel.attempts=1;
        fakeReset=reason; displayHook={}; fakeBusy=1;
        const auto before=connections;
        wake(0);
        assert(connections==before+1 && pendingPanel.magic==0);
    }
    puts("ponretry: detector, cached retry, exact readout inputs, timer cleanup, limit, stale flags and watchdog passed");
}
