// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
#define main baseline_main
#include "test_wake.cpp"
#undef main
State stored() {
    State s{}; Preferences p; p.begin("ventanal",false);
    assert(p.getBytes("state13",&s,sizeof s)==sizeof s); p.end(); return s;
}
void hold(unsigned duration=1600) {
    const auto base=fakeClock;
    digitalReader=[base,duration](int pin) {
        return pin==18 ? (fakeClock-base<duration?LOW:HIGH) : fakeBusy;
    };
}
int main() {
    fakeReset=ESP_RST_POWERON; wake(0); fakeReset=ESP_RST_DEEPSLEEP;
    for(auto screen : {Screen::Art,Screen::Weather,Screen::Solar}) {
        State s=stored(); s.active=screen; s.solar=art;
        strcpy(s.solar.id,"ssssssssssssssss"); manifestExtras.solar=s.solar;
        files["/solar.vnf"]={1};
        s.owner.cursor=1; s.owner.overrideArt=art; files["/override.vnf"]={1};
        s.owner.queue.push("1111111111111111","2222222222222222");
        Preferences p; p.begin("ventanal",false); save(p,s); p.end();
        auto before=connections, reports=manifests, draws=readouts;
        hold(); wake(ESP_SLEEP_WAKEUP_EXT0);
        assert(connections==before+1 && manifests==reports+1 && readouts==draws+1);
        auto after=stored();
        assert(after.active==screen && after.owner.cursor==1 && after.owner.queue.count==0);
        assert(strstr(manifestSkipQuery,"1111111111111111~2222222222222222"));
        // Even unchanged IDs and battery force another refresh, including offline.
        online=false; before=connections; draws=readouts;
        hold(5000); wake(ESP_SLEEP_WAKEUP_EXT0);
        assert(connections==before+1 && readouts==draws+1 && offlineDraw);
        assert(stored().active==screen && stored().owner.cursor==1);
        online=true;
    }
    // PON retry retains the forced-refresh identity and does not go online twice.
    hold(); displayHook=[] { fakeBusy=0; fakeClock=panelStarted+3000; timerCallback(nullptr); };
    fakeWake=ESP_SLEEP_WAKEUP_EXT0;
    const int retryConnections=connections;
    try { setup(); assert(false); } catch(const Restarting&) {}
    assert(connections==retryConnections+1 && panelState().active==Screen::Solar);
    auto retryTime=pendingPanel.drawnAt;
    fakeReset=ESP_RST_SW; fakeClock=0; fakeBusy=1; digitalReader={}; displayHook={};
    wake(0);
    assert(connections==retryConnections+1 && stored().active==Screen::Solar);
    assert(pendingPanel.drawnAt==retryTime && blitted=="/solar.vnf");
    fakeReset=ESP_RST_DEEPSLEEP;
    // Full online transfer path includes new main/weather/solar and spare frames.
    strcpy(art.id,"cccccccccccccccc"); strcpy(weather.id,"dddddddddddddddd");
    strcpy(manifestExtras.solar.id,"eeeeeeeeeeeeeeee");
    manifestExtras.count=1; manifestExtras.spares[0]=art;
    int transferred=downloads; hold(); wake(ESP_SLEEP_WAKEUP_EXT0);
    assert(downloads==transferred+4 && stored().active==Screen::Solar);
    // Critical battery still prevents networking and disables timer wakes.
    display.voltage=3.3f; batteryCount=0; int before=connections;
    hold(); wake(ESP_SLEEP_WAKEUP_EXT0,true); assert(connections==before && charges==1);
    assert(watchdogTimeout==180000 && watchdogArmed);
    puts("longpress: same art/weather/solar, forced unchanged/offline redraw, reports, downloads, battery passed");
}
