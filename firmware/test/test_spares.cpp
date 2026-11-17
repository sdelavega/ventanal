// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
#define main baseline_main
#include "test_wake.cpp"
#undef main
void press(bool twice) {
    const uint32_t base=fakeClock;
    digitalReader=[base,twice](int pin) {
        if(pin!=18) return fakeBusy;
        const auto elapsed=fakeClock-base;
        return elapsed<80 || (twice && elapsed>=250 && elapsed<350)?LOW:HIGH;
    };
}
State stored() {
    State state{}; Preferences prefs; prefs.begin("ventanal",false);
    assert(prefs.getBytes("state13",&state,sizeof state)==sizeof state); prefs.end(); return state;
}
int main() {
    manifestExtras.count=3;
    for(int i=0;i<3;i++) {
        manifestExtras.spares[i]=art;
        memset(manifestExtras.spares[i].id,'c'+i,16);
        manifestExtras.spares[i].readout={100,20,400,30,false};
    }
    // Upgrade the previous NVS layout without losing its active screen.
    struct Legacy { Screen active; ScreenInfo art, weather; Drawn drawn; } legacy{};
    legacy.active=Screen::Weather; legacy.art=art; legacy.weather=weather;
    Preferences initial; initial.begin("ventanal",false);
    initial.putBytes("state13",&legacy,sizeof legacy); initial.end();
    fakeReset=ESP_RST_POWERON; wake(0);
    assert(downloads==5 && files.count("/spare0.vnf") && files.count("/spare2.vnf"));
    int before=connections;
    press(true); wake(ESP_SLEEP_WAKEUP_EXT0);
    auto state=stored();
    assert(connections==before && blitted=="/override.vnf");
    assert(!strcmp(selectedArt(state).id,"cccccccccccccccc") && state.owner.queue.count==1);
    assert(lastReadout.x==100 && lastReadout.y==20 && lastReadout.w==400 && lastReadout.h==30);
    assert(files.count("/spare0.vnf")); // cycling retains the source cache
    press(false); wake(ESP_SLEEP_WAKEUP_EXT0); assert(blitted=="/weather.vnf");
    press(false); wake(ESP_SLEEP_WAKEUP_EXT0); assert(blitted=="/override.vnf");
    press(true); wake(ESP_SLEEP_WAKEUP_EXT0);
    assert(!strcmp(selectedArt(stored()).id,"dddddddddddddddd"));
    // A PON retry redraws this exact override without another press/network.
    online=false; press(true);
    displayHook=[] { fakeBusy=0; fakeClock=panelStarted+3000; timerCallback(nullptr); };
    fakeWake=ESP_SLEEP_WAKEUP_EXT0;
    try { setup(); assert(false); } catch(const Restarting&) {}
    assert(!strcmp(selectedArt(panelState()).id,"eeeeeeeeeeeeeeee"));
    auto retryAt=pendingPanel.drawnAt;
    fakeReset=ESP_RST_SW; fakeClock=0; fakeBusy=1; digitalReader={}; displayHook={};
    wake(0); assert(blitted=="/override.vnf" && pendingPanel.drawnAt==retryAt && connections==before);
    assert(stored().owner.queue.count==3);
    press(true); wake(ESP_SLEEP_WAKEUP_EXT0);
    assert(blitted=="/art.vnf" && stored().owner.cursor==-1);
    // Failed response retains queued reports. Successful response acknowledges.
    digitalReader={}; wake(ESP_SLEEP_WAKEUP_TIMER); assert(stored().owner.queue.count==4);
    online=true; wake(ESP_SLEEP_WAKEUP_TIMER); assert(stored().owner.queue.count==0);
    assert(strstr(manifestSkipQuery,"aaaaaaaaaaaaaaaa~cccccccccccccccc"));
    // Replace the source slot while keeping the displayed override and readout.
    press(true); wake(ESP_SLEEP_WAKEUP_EXT0);
    strcpy(manifestExtras.spares[0].id,"ffffffffffffffff");
    digitalReader={}; wake(ESP_SLEEP_WAKEUP_TIMER);
    assert(!strcmp(stored().owner.spares[0].id,"ffffffffffffffff"));
    assert(!strcmp(selectedArt(stored()).id,"cccccccccccccccc") && files.count("/override.vnf"));
    // A new main generation clears the override, including when showing art.
    strcpy(art.id,"9999999999999999"); wake(ESP_SLEEP_WAKEUP_TIMER);
    assert(stored().owner.cursor==-1 && !stored().owner.overrideArt.id[0] && blitted=="/art.vnf");
    manifestExtras.count=1; wake(ESP_SLEEP_WAKEUP_TIMER);
    assert(!files.count("/spare1.vnf") && !files.count("/spare2.vnf"));
    // An exhausted wake defers spare work until the next wake.
    manifestExtras.count=3; files.erase("/spare0.vnf");
    strcpy(art.id,"8888888888888888");
    wifiDelay=105000;
    auto prior=downloads; wake(ESP_SLEEP_WAKEUP_TIMER);
    assert(downloads==prior && !files.count("/spare0.vnf"));
    wifiDelay=0; wake(ESP_SLEEP_WAKEUP_TIMER); assert(files.count("/spare0.vnf"));
    puts("spares: offline double/single, retained override, queue delivery, PON identity, new slot and deletion passed");
}
