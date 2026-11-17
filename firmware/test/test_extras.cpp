// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
#define main baseline_main
#include "test_wake.cpp"
#undef main
void press(bool) { const uint32_t base=fakeClock; digitalReader=[base](int pin) { return pin==18 && fakeClock-base<80?LOW:HIGH; }; }
State stored() { State s{}; Preferences p; p.begin("ventanal",false); assert(p.getBytes("state13",&s,sizeof s)==sizeof s); p.end(); return s; }
int main() {
    struct SolarState { Screen active; ScreenInfo art,weather; Drawn drawn; OwnerState owner{}; ScreenInfo solar{}; char solarDrawn[17]{}; } old{};
    old.active=Screen::Solar; old.art=art; old.weather=weather; old.solar=weather;
    old.owner.cursor=2; old.owner.queue.push("aaaaaaaaaaaaaaaa","bbbbbbbbbbbbbbbb");
    files["/art.vnf"]={1}; files["/weather.vnf"]={1}; files["/solar.vnf"]={1};
    Preferences p; p.begin("ventanal",false); p.putBytes("state13",&old,sizeof old); p.end();
    online=false; fakeReset=ESP_RST_POWERON; wake(0);
    assert(stored().active==Screen::Solar && stored().owner.cursor==2 && stored().owner.queue.count==1);
    assert(Serial.output.find("quarantined")==std::string::npos);
    online=true;
    manifestExtras.solar=weather;
    manifestExtras.extraScreens[0]=art; manifestExtras.extraScreens[1]=weather;
    strcpy(manifestExtras.names[0],"extra-test"); strcpy(manifestExtras.names[1],"extra-second");
    manifestExtras.order[0]=Screen::Art; manifestExtras.order[1]=Screen::Weather;
    manifestExtras.order[2]=Screen::Solar; manifestExtras.order[3]=Screen::Extra0;
    manifestExtras.order[4]=Screen::Extra1; manifestExtras.orderCount=5;
    wake(ESP_SLEEP_WAKEUP_TIMER);
    assert(files.count("/extra0.vnf") && files.count("/extra1.vnf"));
    press(false); wake(ESP_SLEEP_WAKEUP_EXT0); assert(blitted=="/extra0.vnf");
    assert(Serial.output.find("draw=extra-test")!=std::string::npos);
    press(false); wake(ESP_SLEEP_WAKEUP_EXT0); assert(blitted=="/extra1.vnf");
    press(false); wake(ESP_SLEEP_WAKEUP_EXT0); assert(blitted=="/art.vnf");
    // Follow a nontrivial ordered list rather than enum arithmetic.
    manifestExtras.order[0]=Screen::Art; manifestExtras.order[1]=Screen::Extra0;
    manifestExtras.order[2]=Screen::Weather; manifestExtras.orderCount=3;
    digitalReader={}; wake(ESP_SLEEP_WAKEUP_TIMER);
    press(false); wake(ESP_SLEEP_WAKEUP_EXT0); assert(blitted=="/extra0.vnf");
    manifestExtras.extraScreens[0]={}; manifestExtras.extraScreens[1]={};
    manifestExtras.order[0]=Screen::Art; manifestExtras.order[1]=Screen::Weather; manifestExtras.orderCount=2;
    digitalReader={}; wake(ESP_SLEEP_WAKEUP_TIMER);
    assert(stored().active==Screen::Art && !files.count("/extra0.vnf"));
    // No space for atomic replacement: keep old cache and skip unavailable extra.
    LittleFS.capacity=LittleFS.usedBytes()+960015;
    manifestExtras.extraScreens[0]=art; manifestExtras.order[2]=Screen::Extra0; manifestExtras.orderCount=3;
    wake(ESP_SLEEP_WAKEUP_TIMER); assert(!files.count("/extra0.vnf"));
    press(false); wake(ESP_SLEEP_WAKEUP_EXT0); assert(blitted=="/weather.vnf");
    press(false); wake(ESP_SLEEP_WAKEUP_EXT0); assert(blitted=="/art.vnf");
    puts("extras: generic ordered 3/5-screen cycle, migration, removal and LittleFS bounds passed");
}
