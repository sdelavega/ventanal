// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
#include <cassert>
#include <cstdio>
#include "../ventanal/net.h"
#include "../ventanal/overlay.h"
#include <Preferences.h>
#include <esp_sleep.h>
#include <vector>
static int connections=0, manifests=0, downloads=0, readouts=0, waiting=0, charges=0;
static bool online=true, frameOK=true, offlineDraw=false;
static ScreenInfo art{"aaaaaaaaaaaaaaaa","/frames/art",{1092,22,480,26,false}};
static ScreenInfo weather{"bbbbbbbbbbbbbbbb","/frames/weather",{1236,30,334,136,true}};
static uint32_t wifiDelay=0, timeDelay=0, manifestDelay=0, frameDelay=0;
static std::vector<std::string> downloadPaths;
bool wifiConnect(uint32_t) { ++connections; delay(wifiDelay); return online; }
bool syncTime() { delay(timeDelay); return online; }
bool fetchManifest(ScreenInfo& a,ScreenInfo& w) { ++manifests; delay(manifestDelay); if(!online)return false; a=art;w=weather;return true; }
bool fetchFrame(const char*,const char* path) { ++downloads; downloadPaths.push_back(path); delay(frameDelay); if(frameOK) files[path]={1}; return frameOK; }
static std::string blitted;

bool blitFrame(const char* path,Inkplate&) { blitted=path;return files.count(path); }
static ReadoutRect lastReadout{};
static time_t readoutTime=0;
static int readoutPct=0;
void drawReadout(Inkplate&,const ReadoutRect& rect,int pct,bool offline,bool,time_t at,bool) { lastReadout=rect; ++readouts;offlineDraw=offline;readoutTime=at;readoutPct=pct; }
void drawWaiting(Inkplate&,const char*) { ++waiting; }
void drawChargeMe(Inkplate&) { ++charges; }
#include "../ventanal/ventanal.ino"
static void wake(int cause, bool critical=false) {
    fakeWake=cause; timerUs=0; buttonArmed=false;
    const unsigned before=display.refreshes;
    try { setup(); assert(false); } catch(const Sleeping&) {}
    assert(display.refreshes-before<=1); assert(buttonArmed);
    assert(critical ? timerUs==0 : timerUs>0);
}
int main() {
    for(auto reason : {ESP_RST_TASK_WDT, ESP_RST_INT_WDT, ESP_RST_WDT, ESP_RST_PANIC,
                       ESP_RST_BROWNOUT, ESP_RST_UNKNOWN, ESP_RST_EFUSE, ESP_RST_PWR_GLITCH, ESP_RST_CPU_LOCKUP}) {
        assert(isAbnormalReset(reason));
        fakeReset=reason;
        wake(ESP_SLEEP_WAKEUP_EXT0);
        assert(watchdogTimeout==180000 && watchdogArmed);
        assert(watchdogResets==0 && blitted=="/weather.vnf");
    }
    watchdogResets=2;
    Preferences guard; guard.begin("ventanal",false);
    guard.putBytes("wdt-count", &watchdogResets, sizeof watchdogResets); guard.end();
    const auto refreshes=display.refreshes;
    const auto begins=display.begins;
    fakeReset=ESP_RST_PANIC;
    wake(0);
    assert(watchdogResets==3 && display.refreshes==refreshes && display.begins==begins);
    fakeReset=ESP_RST_DEEPSLEEP;
    wake(ESP_SLEEP_WAKEUP_TIMER);
    assert(watchdogResets==0); // limit sleeps retain count, next timer tries
    preferences.clear(); files.clear(); connections=manifests=downloads=readouts=0;
    fakeReset=ESP_RST_POWERON;

    wake(0); assert(connections==1 && manifests==1 && downloads==2 && readouts==1);
    assert(blitted=="/weather.vnf");
    wake(ESP_SLEEP_WAKEUP_TIMER); assert(connections==2 && manifests==2 && downloads==2 && readouts==1);
    strcpy(weather.id,"cccccccccccccccc");
    wake(ESP_SLEEP_WAKEUP_TIMER); assert(downloads==3 && readouts==2);
    wake(ESP_SLEEP_WAKEUP_EXT0); assert(connections==3 && manifests==3 && downloads==3 && readouts==3);
    assert(blitted=="/art.vnf");
    online=false; wake(ESP_SLEEP_WAKEUP_EXT0); assert(blitted=="/weather.vnf" && connections==3);
    wake(0); assert(offlineDraw && readouts==5);
    files.clear(); wake(ESP_SLEEP_WAKEUP_TIMER); assert(waiting==1);
    online=true; wake(ESP_SLEEP_WAKEUP_TIMER); assert(downloads==5 && readouts==6);
    strcpy(weather.id,"dddddddddddddddd"); frameOK=false;
    wake(ESP_SLEEP_WAKEUP_TIMER); assert(downloads==6 && readouts==6); // No false commit/draw.
    frameOK=true; wake(ESP_SLEEP_WAKEUP_TIMER); assert(downloads==7 && readouts==7);
    // Mount failure must ignore intact cached files, then recover on the next wake.
    LittleFS.failBegin=true;
    int priorDownloads=downloads, priorReadouts=readouts, priorWaiting=waiting;
    wake(0);
    assert(downloads==priorDownloads && readouts==priorReadouts && waiting==priorWaiting+1);
    LittleFS.failBegin=false;
    wake(ESP_SLEEP_WAKEUP_TIMER);
    assert(downloads==priorDownloads && readouts==priorReadouts+1);
    assert(LittleFS.formatAttempts==0);

    // NVS failure degrades to a usable draw without changing persisted state.
    const auto savedPreferences=preferences;
    Preferences::failBegin=true;
    priorDownloads=downloads; priorReadouts=readouts;
    wake(0);
    assert(downloads==priorDownloads+2 && readouts==priorReadouts+1);
    assert(blitted=="/weather.vnf" && preferences==savedPreferences);
    Preferences::failBegin=false;
    wake(ESP_SLEEP_WAKEUP_EXT0);
    assert(blitted=="/art.vnf" && downloads==priorDownloads+2);

    display.voltage=3.3f; batteryCount=0;
    int before=connections; wake(0,true); assert(charges==1 && connections==before && timerUs==0);

    // A replacement draw clears the marker and permits a future critical episode.
    display.voltage=3.92f; batteryCount=0;
    wake(ESP_SLEEP_WAKEUP_TIMER);
    assert(preferences.at(chargeMarker).at(0)==0);
    display.voltage=3.3f; batteryCount=0;
    struct Brownout {};
    displayHook=[] {
        assert(preferences.at(chargeMarker).at(0)==1);
        throw Brownout{};
    };
    try { setup(); assert(false); } catch(const Brownout&) {}
    assert(charges==2 && timerRunning);
    displayHook={};
    // Simulate boot losing RTC battery history and a misleading recovered ADC value.
    timerRunning=false; panelTimer=nullptr; panelRecoveryTask=nullptr;
    batteryCount=0; display.voltage=3.92f; fakeReset=ESP_RST_BROWNOUT;
    before=connections;
    const auto afterBrownout=display.begins;
    wake(0,true);
    assert(charges==2 && connections==before && display.begins==afterBrownout);
    assert(preferences.at("wdt-count").at(0)==1);
    batteryCount=0; display.voltage=3.3f; fakeReset=ESP_RST_DEEPSLEEP;
    wake(ESP_SLEEP_WAKEUP_EXT0,true);
    assert(charges==2 && connections==before);
    const auto beforeReplacement=display.refreshes;
    batteryCount=0; display.voltage=3.92f;
    wake(ESP_SLEEP_WAKEUP_TIMER);
    assert(display.refreshes==beforeReplacement+1 && preferences.at(chargeMarker).at(0)==0);
    batteryCount=0; display.voltage=3.3f;
    preferences.erase(chargeMarker);
    Preferences::failWriteKey=chargeMarker;
    wake(ESP_SLEEP_WAKEUP_EXT0,true);
    assert(charges==2 && !preferences.count(chargeMarker));
    Preferences::failWriteKey="wdt-count";
    fakeReset=ESP_RST_BROWNOUT;
    const auto beforeFailedStrike=display.begins;
    wake(0);
    assert(display.begins==beforeFailedStrike);
    Preferences::failWriteKey.clear();

    // A failed first format keeps the next boot eligible to provision.
    preferences.clear(); files.clear();
    fakeReset=ESP_RST_POWERON; display.voltage=3.92f; batteryCount=0;
    LittleFS.failBegin=LittleFS.failFormat=true;
    priorDownloads=downloads;
    wake(0);
    assert(LittleFS.formatAttempts==1 && !preferences.count("state13") && downloads==priorDownloads);
    LittleFS.failFormat=false;
    wake(ESP_SLEEP_WAKEUP_EXT0);
    assert(LittleFS.formatAttempts==2 && downloads==priorDownloads+2 && blitted=="/weather.vnf");

    // Quarantined state, inaccessible NVS, and an uncertain key lookup all protect flash.
    const auto cachedFiles=files;
    preferences.clear(); preferences["state-bad"]={1};
    LittleFS.failBegin=true;
    wake(0);
    assert(LittleFS.formatAttempts==2 && files==cachedFiles);
    preferences.clear(); Preferences::failBegin=true;
    wake(0);
    assert(LittleFS.formatAttempts==2 && files==cachedFiles);
    Preferences::failBegin=false; failKeyLookup=true;
    wake(0);
    assert(LittleFS.formatAttempts==2 && files==cachedFiles);
    puts("wake cycle: cache provisioning and preservation, reset strikes, charge latch and normal draw passed");
    return 0;
}
