// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
#define main baseline_main
#include "test_wake.cpp"
#undef main
void press(bool) {
    const uint32_t base=fakeClock;
    digitalReader=[base](int pin) { return pin==18 && fakeClock-base<80?LOW:HIGH; };
}
State stored() { State s{}; Preferences p; p.begin("ventanal",false); assert(p.getBytes("state13",&s,sizeof s)==sizeof s); p.end(); return s; }
int main() {
    assert(screenAfterWake(WakeCause::Button,Screen::Art,true)==Screen::Weather);
    assert(screenAfterWake(WakeCause::Button,Screen::Weather,true)==Screen::Solar);
    assert(screenAfterWake(WakeCause::Button,Screen::Solar,true)==Screen::Art);
    assert(screenAfterWake(WakeCause::Button,Screen::Weather,false)==Screen::Art);
    struct Previous { Screen active; ScreenInfo art,weather; Drawn drawn; OwnerState owner{}; } old{};
    static_assert(sizeof(Previous)==1084,"migrate current production ABI");
    old.active=Screen::Art; old.art=art; old.weather=weather;
    old.owner.cursor=1; old.owner.spares[1]=art;
    old.owner.queue.push("aaaaaaaaaaaaaaaa","bbbbbbbbbbbbbbbb");
    Preferences p; p.begin("ventanal",false); p.putBytes("state13",&old,sizeof old); p.end();
    online=false; fakeReset=ESP_RST_POWERON; wake(0);
    auto state=stored();
    assert(state.owner.cursor==1 && state.owner.queue.count==1);
    assert(!strcmp(state.owner.spares[1].id,art.id));
    assert(Serial.output.find("state migrated")!=std::string::npos);
    online=true; manifestExtras.solar=weather; strcpy(manifestExtras.solar.id,"cccccccccccccccc");
    manifestExtras.solar.readout={40,50,300,100,true};
    wake(ESP_SLEEP_WAKEUP_TIMER); assert(files.count("/solar.vnf"));
    press(false); wake(ESP_SLEEP_WAKEUP_EXT0); assert(blitted=="/weather.vnf");
    press(false); wake(ESP_SLEEP_WAKEUP_EXT0); assert(blitted=="/solar.vnf" && stored().active==Screen::Solar);
    assert(lastReadout.x==40 && lastReadout.y==50 && lastReadout.w==300 && lastReadout.h==100);
    online=false; digitalReader={};
    displayHook=[] { fakeBusy=0; fakeClock=panelStarted+3000; timerCallback(nullptr); };
    fakeWake=0; fakeReset=ESP_RST_POWERON;
    try { setup(); assert(false); } catch(const Restarting&) {}
    assert(panelState().active==Screen::Solar);
    fakeReset=ESP_RST_SW; fakeClock=0; fakeBusy=1; digitalReader={}; displayHook={};
    wake(0); assert(blitted=="/solar.vnf" && lastReadout.x==40 && lastReadout.y==50);
    online=true;
    press(false); wake(ESP_SLEEP_WAKEUP_EXT0); assert(blitted=="/art.vnf");
    manifestExtras.solar={}; digitalReader={}; wake(ESP_SLEEP_WAKEUP_TIMER);
    assert(!files.count("/solar.vnf"));
    puts("solar: cycle, optional cache, production State migration and owner preservation passed");
}
