// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
#include <cassert>
#include <cstdio>
#include "../ventanal/net.h"
#include "../ventanal/overlay.h"
#include <Preferences.h>
#include <esp_sleep.h>
#include <esp_system.h>
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
static OtaBoot simulatedOtaBoot;
static bool otaLayout=false;
static unsigned otaConfirmations=0;
OtaBoot readOtaBoot(Preferences&, bool) { return simulatedOtaBoot; }
void acknowledgeOtaReport(Preferences&, OtaBoot&) {}
bool confirmOtaBoot(Preferences&, OtaBoot& boot, bool manifestOk, bool screenDrawn) {
    if(!otaCanConfirm(boot.pending,boot.matching,manifestOk,screenDrawn)) return false;
    ++otaConfirmations;
    return true;
}
void rollbackOtaBoot() { esp_restart(); }
bool otaCacheLayout() { return otaLayout; }
void tryOta(Preferences&, const OtaBoot&, bool, bool, int, int, int) {}
#include "../ventanal/ventanal.ino"
static void simulateBoot(esp_reset_reason_t reason) {
    fakeReset=reason;
    batteryCount=0;
    timerRunning=false;
    registeredTimerCallback=nullptr;
    notifiedTask=nullptr;
    panelTimer=nullptr;
    panelRecoveryTask=nullptr;
}
static void wake(int cause, bool critical=false) {
    fakeWake=cause;
    // Start with both sources enabled when testing that critical sleep disables them.
    timerUs=critical ? 1000 : 0;
    buttonArmed=critical;
    const unsigned before=display.refreshes;
    try { setup(); assert(false); } catch(const Sleeping&) {}
    assert(display.refreshes-before<=1);
    if(critical) {
        assert(!buttonArmed && timerUs==0);
        assert(pwrHeld && deepHold && pwrLevel==0);
    } else {
        assert(buttonArmed && timerUs>0);
    }
}
int main() {
    hostSettingsAvailable=false;
    fakeReset=ESP_RST_POWERON;
    fakeWake=0;
    try { setup(); assert(false); } catch(const Sleeping&) {}
    assert(waiting==1 && connections==0 && manifests==0 && downloads==0);
    assert(buttonArmed && timerUs==0);
    hostSettingsAvailable=true;
    waiting=0;

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
    simulateBoot(ESP_RST_POWERON);
    wake(0);
    assert(preferences.at(chargeMarker).at(0)==0 && !pwrHeld);
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
    simulateBoot(ESP_RST_BROWNOUT);
    display.voltage=3.92f;
    before=connections;
    const auto afterBrownout=display.begins;
    wake(0,true);
    assert(charges==2 && connections==before && display.begins==afterBrownout);
    assert(preferences.at("wdt-count").at(0)==1);
    simulateBoot(ESP_RST_POWERON);
    display.voltage=3.3f;
    wake(0,true);
    assert(charges==2 && connections==before);
    const auto beforeReplacement=display.refreshes;
    simulateBoot(ESP_RST_POWERON);
    display.voltage=3.92f;
    wake(0);
    assert(display.refreshes==beforeReplacement+1 && preferences.at(chargeMarker).at(0)==0);
    batteryCount=0; display.voltage=3.3f;
    preferences.erase(chargeMarker);
    Preferences::failWriteKey=chargeMarker;
    const auto beforeFailedMarker=display.refreshes;
    wake(ESP_SLEEP_WAKEUP_EXT0,true);
    assert(charges==3 && display.refreshes==beforeFailedMarker+1);
    assert(!preferences.count(chargeMarker));
    Preferences::failWriteKey="wdt-count";
    fakeReset=ESP_RST_BROWNOUT;
    const auto beforeFailedStrike=display.begins;
    wake(0);
    assert(display.begins==beforeFailedStrike);
    Preferences::failWriteKey.clear();

    // Read-only NVS distinguishes a missing namespace from an empty existing one.
    preferences.clear(); files.clear();
    preferencesNamespaceExists=false;
    nvs_handle_t handle;
    assert(nvs_open("ventanal",NVS_READONLY,&handle)==ESP_ERR_NVS_NOT_FOUND);
    assert(canProvisionCache() && !preferencesNamespaceExists);
    assert(nvs_open("ventanal",NVS_READWRITE,&handle)==ESP_OK);
    nvs_close(handle);
    assert(preferencesNamespaceExists && preferences.empty() && canProvisionCache());
    failNvsOpen=true;
    assert(!canProvisionCache());
    failNvsOpen=false;
    preferencesNamespaceExists=false;

    // Critical voltage on first boot must leave factory provisioning available.
    simulateBoot(ESP_RST_POWERON);
    display.voltage=3.3f;
    LittleFS.failBegin=true;
    wake(0,true);
    assert(LittleFS.formatAttempts==0 && !preferences.count("state13"));
    assert(preferences.at(chargeMarker).at(0)==1 && canProvisionCache());

    // A failed first format keeps the next boot eligible to provision.
    simulateBoot(ESP_RST_POWERON);
    display.voltage=3.92f;
    LittleFS.failBegin=LittleFS.failFormat=true;
    priorDownloads=downloads;
    wake(0);
    assert(LittleFS.formatAttempts==1 && !preferences.count("state13") && downloads==priorDownloads);
    LittleFS.failFormat=false;
    wake(ESP_SLEEP_WAKEUP_EXT0);
    assert(LittleFS.formatAttempts==2 && downloads==priorDownloads+2 && blitted=="/weather.vnf");
    assert(preferences.count("state13"));

    // Quarantined state, inaccessible NVS, and an uncertain key lookup all protect flash.
    const auto cachedFiles=files;
    preferences.clear(); preferences["state-bad"]={1};
    LittleFS.failBegin=true;
    wake(0);
    assert(LittleFS.formatAttempts==2 && files==cachedFiles && !preferences.count("state13"));
    preferences.clear(); Preferences::failBegin=true;
    wake(0);
    assert(LittleFS.formatAttempts==2 && files==cachedFiles && !preferences.count("state13"));
    Preferences::failBegin=false; failKeyLookup=true;
    wake(0);
    assert(LittleFS.formatAttempts==2 && files==cachedFiles && !preferences.count("state13"));
    failKeyLookup=false;
    wake(0);
    assert(LittleFS.formatAttempts==3 && preferences.count("state13"));

    // A normal boot with both NVS and LittleFS blank provisions in one wake.
    preferences.clear(); preferencesNamespaceExists=false; files.clear();
    LittleFS.failBegin=true;
    simulateBoot(ESP_RST_POWERON);
    priorDownloads=downloads;
    wake(0);
    assert(LittleFS.formatAttempts==4 && preferences.count("state13"));
    assert(downloads==priorDownloads+2 && blitted=="/weather.vnf");
    // A wired move initializes the new cache even with the old NVS state.
    otaLayout=true;
    State oldCacheState{};
    memcpy(&oldCacheState,preferences.at("state13").data(),sizeof oldCacheState);
    oldCacheState.active=Screen::Art;
    oldCacheState.owner.overrideArt=art;
    oldCacheState.owner.cursor=1;
    std::vector<uint8_t> oldStateBytes(sizeof oldCacheState);
    memcpy(oldStateBytes.data(),&oldCacheState,sizeof oldCacheState);
    preferences["state13"]=oldStateBytes;
    LittleFS.failBegin=true;
    simulateBoot(ESP_RST_POWERON);
    wake(0);
    assert(LittleFS.formatAttempts==5 && preferences.count("cache-ota"));
    assert(preferences.count("state13") && files.count("/weather.vnf"));
    State migratedState{};
    memcpy(&migratedState,preferences.at("state13").data(),sizeof migratedState);
    assert(blitted=="/art.vnf" && !migratedState.owner.overrideArt.id[0]);
    LittleFS.failBegin=true;
    wake(0);
    assert(LittleFS.formatAttempts==5);
    LittleFS.failBegin=false;

    // Confirm after a completed frame refresh on the new slot.
    simulatedOtaBoot.pending=simulatedOtaBoot.matching=true;
    simulateBoot(ESP_RST_SW);
    displayHook=[] { assert(otaConfirmations==0); };
    wake(0);
    displayHook={};
    assert(otaConfirmations==1 && !otaPendingBoot);

    // A cached frame drawn offline can't confirm the new app.
    online=false;
    preferences["wdt-count"]={1};
    simulateBoot(ESP_RST_SW);
    try { wake(0); assert(false); } catch(const Restarting&) {}
    assert(otaConfirmations==1 && otaPendingBoot && preferences.at("wdt-count").at(0)==1);
    online=true;

    // The old slot must recover even when the failed image tripped a watchdog
    // and the stored count would otherwise block a normal draw.
    simulatedOtaBoot={};
    simulatedOtaBoot.rolledBack=true;
    preferences["wdt-count"]={2};
    simulateBoot(ESP_RST_TASK_WDT);
    before=connections;
    wake(0);
    assert(connections==before+1 && preferences.at("wdt-count").at(0)==0);
    puts("wake cycle: cache provisioning and preservation, reset strikes, charge latch and normal draw passed");
    return 0;
}
