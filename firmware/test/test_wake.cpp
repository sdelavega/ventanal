// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
#include <cassert>
#include <cstdio>
#include "../ventanal/cache.h"
#include "../ventanal/net.h"
#include "../ventanal/security.h"
#include "../ventanal/overlay.h"
#include "../ventanal/sd_card.h"
#include <Preferences.h>
#include <esp_rtc_time.h>
#include <esp_sleep.h>
#include <esp_system.h>
#include <vector>
static int connections=0, manifests=0, downloads=0, readouts=0, waiting=0, charges=0;
static bool online=true, frameOK=true, offlineDraw=false;
static unsigned transientManifestFailures=0;
static bool otaFails=false;
static bool fetchedManifest=false, recoverWeather=false;
static unsigned frameSweeps=0;
static std::string waitingLabel;
static ScreenInfo art{"aaaaaaaaaaaaaaaa","/frames/art",{1092,22,480,26,false}};
static ScreenInfo weather{"bbbbbbbbbbbbbbbb","/frames/weather",{1236,30,334,136,true}};
static uint32_t wifiDelay=0, timeDelay=0, manifestDelay=0, frameDelay=0;
static std::vector<std::string> downloadPaths;
static std::string failPath; // A frame that never completes (e.g. art on a weak link).
static std::vector<uint32_t> downloadBudgets;
static unsigned sdProbes = 0;
static bool sdPresent = false, sdSelected = false;
static bool otaAccepted = false;
static uint64_t sdTimeUs = 0;
bool mountSdCard() { ++sdProbes; return sdPresent; }
void unmountSdCard() { sdSelected = false; }
bool partialsOnFlash() { return !sdSelected; }
uint64_t esp_rtc_get_time_us() { return sdTimeUs; }
void selectPartialStore(bool mounted) {
    sdSelected = mounted;
    Serial.println(mounted ? "storage partials=sd" : "storage partials=flash");
}
bool wifiConnect(uint32_t) { ++connections; delay(wifiDelay); return online; }
bool syncTime() { delay(timeDelay); return online; }
bool fetchManifest(ScreenInfo& a,ScreenInfo& w,bool* transient) {
    ++manifests;
    fetchedManifest=false;
    delay(manifestDelay);
    if(transient) *transient=transientManifestFailures!=0;
    if(transientManifestFailures) { --transientManifestFailures; return false; }
    if(!online) return false;
    a=art; w=weather;
    fetchedManifest=true;
    return true;
}
uint8_t sweepFramePartials(const ManifestFrameSlots& frames) {
    assert(fetchedManifest && !strcmp(frames[0]->id, art.id) && !strcmp(frames[1]->id, weather.id));
    ++frameSweeps;
    if (!recoverWeather) return 0;
    files["/weather.vnf"]={2};
    return 1u << 1;
}
bool fetchFrame(const char*, const char* path, uint32_t budget) {
    ++downloads;
    downloadPaths.push_back(path);
    downloadBudgets.push_back(budget);
    delay(std::min(frameDelay, budget));
    if (!frameOK || frameDelay > budget || failPath == path) return false;
    files[path] = {1};
    return true;
}
bool canStageExtraFrame(const char*) { return LittleFS.totalBytes() >= LittleFS.usedBytes() + 960016 + 65536; }
static std::string blitted;
static std::string invalidPath;

bool blitFrame(const char* path,Inkplate&) { blitted=path;return files.count(path) && invalidPath != path; }
static ReadoutRect lastReadout{};
static time_t readoutTime=0;
static int readoutPct=0;
void drawReadout(Inkplate&,const ReadoutRect& rect,int pct,bool offline,bool,time_t at,bool) { lastReadout=rect; ++readouts;offlineDraw=offline;readoutTime=at;readoutPct=pct; }
static unsigned downloadFailuresDrawn=0;
void drawDownloadFailure(Inkplate&) { ++downloadFailuresDrawn; }
void drawWaiting(Inkplate&,const char* label) { ++waiting; waitingLabel=label; }
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
bool tryOta(Preferences&, const OtaBoot&, bool, bool, int, int, int,
            void (*prepareStorage)(), void (*)()) {
    if (otaAccepted) prepareStorage();
    return otaFails;
}
#include "../ventanal/ventanal.cpp"
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
        assert(buttonArmed && (timerUs>0)==hostSettingsAvailable);
    }
}
static void weather_fetched_first_when_due() {
    manifestExtras.count=1;
    manifestExtras.spares[0]=art;
    preferences["dl-next"]={5};
    wake(0);
    // Visible screens precede spares; the spare cursor only rotates spares.
    assert((downloadPaths==std::vector<std::string>{"/weather.vnf", "/art.vnf", "/spare0.vnf"}));
}

static State storedState() {
    State state{};
    assert(preferences.at("state13").size()==sizeof state);
    memcpy(&state,preferences.at("state13").data(),sizeof state);
    return state;
}

// Art (the scheduled screen) never finishes while weather is complete:
// the panel shows weather, never "waiting for host", and art stays scheduled.
static void complete_screen_shown_instead_of_waiting() {
    failPath="/art.vnf";
    wake(0);
    assert(files.count("/weather.vnf") && !files.count("/art.vnf") && blitted=="/weather.vnf");
    fakeReset=ESP_RST_DEEPSLEEP;
    Serial.output.clear();
    wake(ESP_SLEEP_WAKEUP_EXT0); // The button schedules art, which is missing.
    assert(waiting==0 && blitted=="/weather.vnf" && readouts==2);
    assert(storedState().active==Screen::Art);
    assert(Serial.output.find("draw=weather")!=std::string::npos);
    assert(Serial.output.find("draw=waiting-for-host")==std::string::npos);
    downloadPaths.clear();
    wake(ESP_SLEEP_WAKEUP_TIMER);
    assert(waiting==0 && storedState().active==Screen::Art);
    assert(!downloadPaths.empty() && downloadPaths.front()=="/art.vnf");
    assert(readouts==2); // The same fallback is already on the panel.
    failPath.clear();
    downloadPaths.clear();
    wake(ESP_SLEEP_WAKEUP_TIMER);
    assert(downloadPaths.front()=="/art.vnf" && blitted=="/art.vnf" && readouts==3 && waiting==0);
    // With nothing complete at all, the waiting screen is still honest.
    files.clear(); online=false;
    wake(ESP_SLEEP_WAKEUP_TIMER);
    assert(waiting==1);
}

// The scheduled screen downloads first, then due weather, then spares in
// round-robin from the saved cursor.
static void displayed_missing_screen_downloads_before_spares() {
    failPath="/art.vnf";
    wake(0);
    fakeReset=ESP_RST_DEEPSLEEP;
    wake(ESP_SLEEP_WAKEUP_EXT0);
    assert(storedState().active==Screen::Art && !files.count("/art.vnf"));
    manifestExtras.count=3;
    for(int i=0;i<3;++i) { manifestExtras.spares[i]=art; memset(manifestExtras.spares[i].id,'1'+i,16); }
    strcpy(weather.id,"cccccccccccccccc"); // Weather is due as well.
    preferences["dl-next"]={6};
    failPath.clear();
    downloadPaths.clear();
    wake(ESP_SLEEP_WAKEUP_TIMER);
    assert((downloadPaths==std::vector<std::string>{"/art.vnf", "/weather.vnf",
                                                    "/spare1.vnf", "/spare2.vnf", "/spare0.vnf"}));
    assert(blitted=="/art.vnf");
    // The first frame keeps the whole remaining wake budget (round-2 H1).
    files.erase("/art.vnf"); files.erase("/spare1.vnf");
    strcpy(art.id,"9999999999999999");
    downloadBudgets.clear(); downloadPaths.clear();
    fakeClock=0;
    wake(ESP_SLEEP_WAKEUP_TIMER);
    assert(downloadPaths.front()=="/art.vnf" && downloadBudgets.front()>frameBudgetMs);
    assert(downloadPaths.back()=="/spare1.vnf");
}

static void invalid_cached_frame_falls_back_and_redownloads() {
    wake(0);
    fakeReset=ESP_RST_DEEPSLEEP;
    invalidPath="/weather.vnf";
    auto state = storedState(); state.drawn.any = false;
    memcpy(preferences["state13"].data(), &state, sizeof state);
    wake(ESP_SLEEP_WAKEUP_TIMER);
    assert(blitted=="/art.vnf" && waiting==0);
    assert(!storedState().weather.id[0]);
    invalidPath.clear(); downloadPaths.clear();
    wake(ESP_SLEEP_WAKEUP_TIMER);
    assert(std::find(downloadPaths.begin(), downloadPaths.end(), "/weather.vnf") != downloadPaths.end());
}
static void zlib_frame_not_started_without_inflate_reserve() {
    expectedHashes = {{weather.url, std::string(64, 'a'), weather.id, true, 100}};
    wifiDelay=100000;
    wake(0);
    assert(std::find(downloadPaths.begin(), downloadPaths.end(), "/weather.vnf") == downloadPaths.end());
}
static void first_frame_uses_remaining_budget() {
    wifiDelay=20000;
    frameDelay=45000;
    wake(0);
    assert(downloadBudgets.front()>83000 && downloadBudgets.front()<85000);
    assert(files.count("/weather.vnf"));
}

static void later_frames_get_leftover_budget() {
    frameDelay=10000;
    wake(0);
    assert(downloadBudgets.size()==2 && downloadBudgets[1]==30000);
    fakeReset=ESP_RST_DEEPSLEEP;
    fakeClock=0;
    downloadBudgets.clear();
    files.clear();
    frameDelay=90000;
    wake(ESP_SLEEP_WAKEUP_TIMER);
    assert(downloadBudgets.size()==2 && downloadBudgets[1]>13000 && downloadBudgets[1]<15000);
    assert(files.count("/weather.vnf") && !files.count("/art.vnf"));
}

static void sd_not_probed_when_nothing_due() {
    sdPresent=true;
    wake(0);
    assert(sdProbes==1);
    fakeReset=ESP_RST_DEEPSLEEP;
    Serial.output.clear();
    wake(ESP_SLEEP_WAKEUP_TIMER);
    assert(sdProbes==1 && Serial.output.find("storage partials=")==std::string::npos);
    strcpy(weather.id,"cccccccccccccccc");
    wake(ESP_SLEEP_WAKEUP_TIMER);
    assert(sdProbes==2 && Serial.output.find("storage partials=sd\n")!=std::string::npos);
    otaAccepted=true;
    const auto priorDownloads=downloads;
    wake(ESP_SLEEP_WAKEUP_TIMER);
    assert(sdProbes==3 && downloads==priorDownloads);
}

static void no_card_result_cached_until_cold_boot_or_24h() {
    constexpr uint64_t dayUs=24ULL*60*60*1000000;
    wake(0);
    assert(sdProbes==1 && Serial.output.find("storage partials=flash\n")!=std::string::npos);
    fakeReset=ESP_RST_DEEPSLEEP;
    sdTimeUs=dayUs-1;
    Serial.output.clear();
    files.clear();
    wake(ESP_SLEEP_WAKEUP_TIMER);
    assert(sdProbes==1 && Serial.output.find("storage partials=")==std::string::npos);
    sdTimeUs=dayUs;
    files.clear();
    wake(ESP_SLEEP_WAKEUP_TIMER);
    assert(sdProbes==2);
    fakeReset=ESP_RST_POWERON;
    sdTimeUs=0;
    files.clear();
    wake(0);
    assert(sdProbes==3);
}

int main(int argc, char** argv) {
    if (argc>1) {
        const std::pair<const char*, void (*)()> cases[]{
            {"weather_fetched_first_when_due", weather_fetched_first_when_due},
            {"invalid_cached_frame_falls_back_and_redownloads", invalid_cached_frame_falls_back_and_redownloads},
            {"zlib_frame_not_started_without_inflate_reserve", zlib_frame_not_started_without_inflate_reserve},
            {"first_frame_uses_remaining_budget", first_frame_uses_remaining_budget},
            {"later_frames_get_leftover_budget", later_frames_get_leftover_budget},
            {"sd_not_probed_when_nothing_due", sd_not_probed_when_nothing_due},
            {"no_card_result_cached_until_cold_boot_or_24h", no_card_result_cached_until_cold_boot_or_24h},
            {"complete_screen_shown_instead_of_waiting", complete_screen_shown_instead_of_waiting},
            {"displayed_missing_screen_downloads_before_spares", displayed_missing_screen_downloads_before_spares},
        };
        for (const auto& [name, run] : cases) {
            if (strcmp(argv[1], name)) continue;
            run();
            puts(name);
            return 0;
        }
        return 1;
    }
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
    transientManifestFailures=1;
    const int sleepsBeforeOta=lightSleeps;
    displayHook=[] { assert(otaConfirmations==0); };
    wake(0);
    displayHook={};
    assert(otaConfirmations==1 && !otaPendingBoot && lightSleeps==sleepsBeforeOta+1);

    // A cached frame drawn offline can't confirm the new app.
    online=false;
    preferences["wdt-count"]={1};
    simulateBoot(ESP_RST_SW);
    try { wake(0); assert(false); } catch(const Restarting&) {}
    assert(otaConfirmations==1 && otaPendingBoot && lightSleeps==sleepsBeforeOta+2
           && preferences.at("wdt-count").at(0)==1);
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
    simulatedOtaBoot={};
    otaFails=true;
    files.erase("/weather.vnf");
    const int downloadsBeforeOta=downloads;
    wake(0);
    assert(downloads==downloadsBeforeOta);
    otaFails=false;

    strcpy(weather.id,"eeeeeeeeeeeeeeee");
    recoverWeather=true;
    const auto sweepsBeforeRecovery=frameSweeps;
    online=false;
    wake(0);
    assert(frameSweeps==sweepsBeforeRecovery && !files.count("/weather.vnf"));
    online=true;
    wake(0);
    assert(frameSweeps==sweepsBeforeRecovery+1 && downloads==downloadsBeforeOta);
    assert(files.at("/weather.vnf").front()==2);
    State recoveredState{};
    memcpy(&recoveredState,preferences.at("state13").data(),sizeof recoveredState);
    assert(!strcmp(recoveredState.weather.id,weather.id));
    recoverWeather=false;

    // A due frame can remain unfinished on a weak link without changing
    // the panel until the 24-hour deadline. A fresh frame removes the badge.
    strcpy(weather.id,"ffffffffffffffff");
    frameOK=false;
    const auto priorBadges=downloadFailuresDrawn;
    const uint32_t now=ventanal::trustedEpoch(time(nullptr));
    wake(ESP_SLEEP_WAKEUP_TIMER);
    assert(contentStaleness.due && downloadFailuresDrawn==priorBadges);
    assert(contentStaleness.overdueSince>=now);
    contentStaleness.overdueSince=now-ventanal::kContentTimeout+60;
    wake(ESP_SLEEP_WAKEUP_TIMER);
    assert(contentStaleness.due && downloadFailuresDrawn==priorBadges);
    contentStaleness.overdueSince=now-ventanal::kContentTimeout;
    wake(ESP_SLEEP_WAKEUP_TIMER);
    assert(downloadFailureDrawn && downloadFailuresDrawn==priorBadges+1);
    frameOK=true;
    wake(ESP_SLEEP_WAKEUP_TIMER);
    assert(!contentStaleness.due && !contentStaleness.overdueSince);
    assert(!downloadFailureDrawn && downloadFailuresDrawn==priorBadges+1);

    hostSettingsAvailable=false;
    simulateBoot(ESP_RST_POWERON);
    wake(0);
    assert(waitingLabel=="Not set up" && buttonArmed && timerUs==0);
    preferences["wdt-count"]={2};
    simulateBoot(ESP_RST_TASK_WDT);
    const int waitingBeforeResetLimit=waiting;
    wake(0);
    assert(waiting==waitingBeforeResetLimit && buttonArmed && timerUs==0);
    hostSettingsAvailable=true;

    puts("wake cycle: cache provisioning and preservation, reset strikes, charge latch and normal draw passed");
    return 0;
}
