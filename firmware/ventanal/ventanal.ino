// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
#include <Inkplate.h>
#include <LittleFS.h>
#include <WiFi.h>
#include <Preferences.h>
#include "battery.h"
#include <nvs.h>
#include <esp_sleep.h>
#include <esp_system.h>
#include <esp_task_wdt.h>
#include <esp_timer.h>
#include <Wire.h>
#ifndef VENTANAL_HOST_TEST
#include <system/inkplateSemaphore.h>
#endif
#include <driver/rtc_io.h>
#include <driver/gpio.h>
#ifdef VENTANAL_HOST_TEST
#include "config.example.h"
#else
#include "config.h"
#endif
#include "security_config.h"
#ifndef VENTANAL_HOST_TEST
// TLS certificate rejection uses more stack than a successful connection.
// Reserve margin for mbedTLS verification and the existing wake State.
SET_LOOP_TASK_STACK_SIZE(16384);
#endif
#include "net.h"
#include "quiet.h"
#include "owner.h"
#include <memory>
#include <new>
#include <type_traits>
#include "cache.h"
#include "overlay.h"

// 1: full application with 60-second sleeps. 2: additionally exercise the
// button application path on the second timer wake (no GPIO press claimed).
#ifndef VENTANAL_WAKETEST
#define VENTANAL_WAKETEST 0
#endif
#ifndef VENTANAL_STATELOSTTEST
#define VENTANAL_STATELOSTTEST 0
#endif
#if VENTANAL_STATELOSTTEST
bool statelostForce=false;
#else
constexpr bool statelostForce=false;
#endif
#ifndef VENTANAL_LONGPRESSTEST
#define VENTANAL_LONGPRESSTEST 0
#endif
#ifndef VENTANAL_SPARESTEST
#define VENTANAL_SPARESTEST 0
#endif
#ifndef VENTANAL_ERASE_CACHE
#define VENTANAL_ERASE_CACHE 0
#endif
#ifndef VENTANAL_HANGTEST
#define VENTANAL_HANGTEST 0
#endif
#ifndef VENTANAL_STALLTEST
#define VENTANAL_STALLTEST 0
#endif
#ifndef VENTANAL_PWRENTEST
#define VENTANAL_PWRENTEST 0
#endif
#ifndef VENTANAL_BOOTTIMETEST
#define VENTANAL_BOOTTIMETEST 0
#endif
#ifndef VENTANAL_BOOT_CONFIRM_MS
#define VENTANAL_BOOT_CONFIRM_MS 0
#endif
#ifndef VENTANAL_PONRETRYTEST
#define VENTANAL_PONRETRYTEST 0
#endif
#if VENTANAL_PONRETRYTEST && (!VENTANAL_STALLTEST || VENTANAL_BOOTTIMETEST || VENTANAL_PWRENTEST)
#error PONRETRYTEST requires STALLTEST with other panel experiments off
#endif
#if VENTANAL_BOOTTIMETEST && (!VENTANAL_STALLTEST || VENTANAL_PWRENTEST)
#error BOOTTIMETEST requires STALLTEST and PWRENTEST off
#endif
Inkplate display;
uint8_t watchdogResets=0;
namespace {
esp_task_wdt_user_handle_t wakeWatchdog;
void armWakeWatchdog() {
    // A user subscription is never fed by Arduino's loop/task watchdog resets.
    const esp_task_wdt_config_t config={(VENTANAL_HANGTEST || VENTANAL_STALLTEST) ? 60000U : 180000U, 0, true};
    const esp_err_t result=esp_task_wdt_init(&config);
    if(result==ESP_ERR_INVALID_STATE) ESP_ERROR_CHECK(esp_task_wdt_reconfigure(&config));
    else ESP_ERROR_CHECK(result);
    ESP_ERROR_CHECK(esp_task_wdt_add_user("ventanal-wake", &wakeWatchdog));
}
void refreshPanel();
void clearWatchdogCount(Preferences& prefs) {
    if(watchdogResets) {
        completeWake(watchdogResets);
        if(prefs.putBytes("wdt-count", &watchdogResets, sizeof watchdogResets)!=sizeof watchdogResets)
            Serial.println("error=nvs-watchdog-write");
    }
}
}
RTC_DATA_ATTR float batteryHistory[4] = {};
RTC_DATA_ATTR uint8_t batteryCount = 0;
#if VENTANAL_WAKETEST == 2 || VENTANAL_SPARESTEST
RTC_DATA_ATTR uint8_t testWakeCount = 0;
#endif

void sleepHardware(uint32_t seconds);
namespace {
struct SolarState {
    Screen active;
    ScreenInfo art;
    ScreenInfo weather;
    Drawn drawn;
    OwnerState owner{};
    ScreenInfo solar{};
    char solarDrawn[17]{};
};
struct State {
    Screen active;
    ScreenInfo art;
    ScreenInfo weather;
    Drawn drawn;
    OwnerState owner{};
    ScreenInfo solar{};
    char solarDrawn[17]{};
    ScreenInfo extraScreens[2]{};
    char extraNames[2][49]{};
    char extraDrawn[2][17]{};
    Screen order[5]{Screen::Art,Screen::Weather};
    uint8_t orderCount=2;
};
void sleepFor(uint32_t seconds, const State& state);
void logState(const char* phase, const State& state, size_t length=0, size_t got=0) {
    Serial.printf("state phase=%s size=%u length=%u got=%u queue=%u cursor=%d override=%.16s spares=%.16s,%.16s,%.16s\n",
        phase,unsigned(sizeof state),unsigned(length),unsigned(got),unsigned(state.owner.queue.count),
        state.owner.cursor,state.owner.overrideArt.id,state.owner.spares[0].id,
        state.owner.spares[1].id,state.owner.spares[2].id);
}
void sleepFor(uint32_t seconds, const State& state) {
    logState("sleep",state);
    sleepHardware(seconds);
}
bool quarantineState(size_t length) {
    std::unique_ptr<uint8_t[]> raw(new(std::nothrow) uint8_t[length ? length : 1]);
    if(!raw) return false;
    nvs_handle_t handle;
    if(nvs_open("ventanal",NVS_READWRITE,&handle)!=ESP_OK) return false;
    size_t got=length;
    bool ok=nvs_get_blob(handle,"state13",raw.get(),&got)==ESP_OK && got==length;
    if(ok) ok=nvs_set_blob(handle,"state-bad",raw.get(),length)==ESP_OK;
    if(ok) ok=nvs_commit(handle)==ESP_OK;
    nvs_close(handle);
    if(ok) Serial.printf("error=state-load quarantined bytes=%u\n",unsigned(length));
    return ok;
}
const ScreenInfo& selectedArt(const State& state) {
    return state.owner.overrideArt.id[0]?state.owner.overrideArt:state.art;
}
const char* screenName(const State& state) {
    if(state.active==Screen::Extra0 || state.active==Screen::Extra1) return state.extraNames[static_cast<unsigned>(state.active)-3];
    return state.active==Screen::Art?"art":state.active==Screen::Solar?"solar":"weather";
}
const ScreenInfo& selectedScreen(const State& state) {
    if(state.active==Screen::Extra0 || state.active==Screen::Extra1) return state.extraScreens[static_cast<unsigned>(state.active)-3];
    return state.active==Screen::Art?selectedArt(state):state.active==Screen::Solar?state.solar:state.weather;
}
const char* selectedArtPath(const State& state) {
    return state.owner.overrideArt.id[0]?"/override.vnf":"/art.vnf";
}
const char* selectedPath(const State& state) {
    if(state.active==Screen::Extra0) return "/extra0.vnf";
    if(state.active==Screen::Extra1) return "/extra1.vnf";
    return state.active==Screen::Art?selectedArtPath(state):state.active==Screen::Solar?"/solar.vnf":"/weather.vnf";
}
const char* selectedDrawn(const State& state) {
    if(state.active==Screen::Extra0 || state.active==Screen::Extra1) return state.extraDrawn[static_cast<unsigned>(state.active)-3];
    return state.active==Screen::Art?state.drawn.artId:state.active==Screen::Solar?state.solarDrawn:state.drawn.weatherId;
}
bool screenCached(const State& state, Screen screen) {
    State candidate=state; candidate.active=screen;
    return selectedScreen(candidate).id[0] && LittleFS.exists(selectedPath(candidate));
}
Screen cycleScreen(const State& state, WakeCause wake, bool mounted) {
    if(wake!=WakeCause::Button) return state.active;
    unsigned start=state.orderCount-1;
    for(unsigned i=0;i<state.orderCount;++i) if(state.order[i]==state.active) start=i;
    for(unsigned i=1;i<=state.orderCount;++i) {
        Screen next=state.order[(start+i)%state.orderCount];
        if(next==Screen::Art || next==Screen::Weather || (mounted && screenCached(state,next))) return next;
    }
    return Screen::Art;
}
bool copyOverride(const char* path) {
    std::unique_ptr<uint8_t[]> buffer(new(std::nothrow) uint8_t[65536]);
    if(!buffer) return false;
    File source=LittleFS.open(path,"r"), out=LittleFS.open("/override.vnf.tmp","w");
    bool ok=bool(source) && bool(out);
    const size_t size=ok?source.size():0;
    size_t copied=0;
    const uint32_t started=millis();
    while(ok && copied<size && millis()-started<30000) {
        size_t n=std::min(size-copied,size_t(65536));
        ok=source.read(buffer.get(),n)==n && out.write(buffer.get(),n)==n;
        if(ok) copied+=n;
    }
    source.close(); out.flush(); out.close();
    ok=ok && copied==size && LittleFS.rename("/override.vnf.tmp","/override.vnf");
    if(!ok) LittleFS.remove("/override.vnf.tmp");
    return ok;
}
// NOINIT survives esp_restart; only a software reset with our magic and a
// bounded attempt count may consume it. No network settings are retained.
struct PendingPanel {
    uint32_t magic;
    uint8_t attempts;
    uint8_t kind; // 0=cached frame, 1=waiting, 2=charge-me
    bool offline;
    int pct;
    time_t drawnAt;
    unsigned char stateBytes[sizeof(State)];
    uint32_t stateChecksum;
    uint8_t sample;
};
static_assert(std::is_trivially_default_constructible<PendingPanel>::value,
              "RTC NOINIT must never run startup initialization");
static_assert(std::is_trivially_copyable<State>::value,"State snapshot uses byte copies");
RTC_NOINIT_ATTR PendingPanel pendingPanel;
uint32_t panelChecksum() {
    uint32_t hash=2166136261U;
    for(auto byte:pendingPanel.stateBytes) hash=(hash^byte)*16777619U;
    return hash;
}
State panelState() {
    State state;
    memcpy(&state,pendingPanel.stateBytes,sizeof state);
    return state;
}
constexpr uint32_t ponMagic=0x504f4e31;
uint32_t panelStarted;
bool panelSeenHigh;
void rememberPanel(const State& state, int pct, bool offline, time_t drawnAt,
                   uint8_t kind=0, uint8_t sample=0) {
    pendingPanel.magic=0; pendingPanel.attempts=0;
    pendingPanel.kind=kind; pendingPanel.offline=offline;
    pendingPanel.pct=pct; pendingPanel.drawnAt=drawnAt; pendingPanel.sample=sample;
    memcpy(pendingPanel.stateBytes,&state,sizeof state);
    pendingPanel.stateChecksum=panelChecksum();
}
void checkPanel(void*) {
    // Never reconfigure BUSYN: the library owns its INPUT_PULLUP mode.
    const uint32_t elapsed=uint32_t(millis()-panelStarted);
    // Ignore power-up/reset pulses; only the post-reset PON window counts.
    const bool high=!statelostForce && elapsed>=500 && elapsed<=3000 && digitalRead(7)==HIGH;
#if VENTANAL_PONRETRYTEST
    if(high && !panelSeenHigh)
        Serial.printf("panel busy-high ms=%lu\n",static_cast<unsigned long>(elapsed));
#endif
    panelSeenHigh=panelSeenHigh || high;
    if(!ponStalled(elapsed,panelSeenHigh)) return;
    if(!nextPonRetry(pendingPanel.attempts)) {
        pendingPanel.magic=0;
        Serial.println("panel pon-retry limit");
        // Do not call the driver's powerOff(), which waits on stuck BUSYN.
        pinMode(21,OUTPUT); digitalWrite(21,LOW);
        sleepFor(pendingPanel.kind==2 ? 0 : secondsUntilNextWake(time(nullptr),VENTANAL_WAKE_MINUTES,
            quietStart(VENTANAL_QUIET_START),quietEnd(VENTANAL_QUIET_END),isLow(pendingPanel.pct)),panelState());
        return;
    }
    pendingPanel.magic=ponMagic;
    Serial.printf("panel pon-stall restart attempt=%u\n",unsigned(pendingPanel.attempts));
    Serial.flush();
    esp_restart();
}
void refreshPanel() {
#if VENTANAL_HANGTEST
    Serial.println("diagnostic=hang-before-display");
    Serial.flush();
    while(true) delay(1);
#endif
    esp_timer_handle_t timer=nullptr;
    esp_timer_create_args_t args={};
    args.callback=checkPanel;
    args.name="panel-pon";
    ESP_ERROR_CHECK(esp_timer_create(&args,&timer));
    panelSeenHigh=false;
    panelStarted=millis();
    ESP_ERROR_CHECK(esp_timer_start_periodic(timer,50000));
#if VENTANAL_PONRETRYTEST
    Serial.printf("panel display-start attempt=%u boot_ms=%lu\n",unsigned(pendingPanel.attempts),static_cast<unsigned long>(panelStarted));
#else
    Serial.printf("panel display-start attempt=%u\n",unsigned(pendingPanel.attempts));
#endif
    if(statelostForce) {
        Serial.println("diagnostic=forced-pon-stall");
        while(true) delay(1);
    }
    display.display();
    ESP_ERROR_CHECK(esp_timer_stop(timer));
    ESP_ERROR_CHECK(esp_timer_delete(timer));
    pendingPanel.magic=0;
    completePonRetry(pendingPanel.attempts);
}
void save(Preferences& prefs, const State& state) {
    logState("save",state);
    if(prefs.putBytes("state13", &state, sizeof state) != sizeof state)
        Serial.println("error=nvs-state-write");
}
void update(Preferences& prefs, State& state, ScreenInfo& cached,
            const ScreenInfo& incoming, const char* path) {
    if(!strcmp(cached.id,incoming.id) && LittleFS.exists(path)) {
        Serial.printf("download path=%s skipped=unchanged\n",path);
        return;
    }
    const String url=String(VENTANAL_HOST_URL)+incoming.url;
    if(fetchFrame(url.c_str(),path)) {
        cached=incoming;
        save(prefs,state); // An ID advances only after an atomic frame commit.
    }
}
}
void sleepHardware(uint32_t seconds) {
#if VENTANAL_STATELOSTTEST
    if(seconds) seconds=10;
#endif
#if VENTANAL_SPARESTEST || VENTANAL_LONGPRESSTEST
    if(seconds) seconds=60;
#endif
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    rtc_gpio_pullup_en(GPIO_NUM_18);
    rtc_gpio_pulldown_dis(GPIO_NUM_18);
    esp_sleep_enable_ext0_wakeup(GPIO_NUM_18, 0);
    if(seconds) esp_sleep_enable_timer_wakeup(uint64_t(seconds)*1000000ULL);
    Serial.printf("sleep seconds=%lu button=GPIO18/LOW\n",static_cast<unsigned long>(seconds));
    Serial.printf("stack min-free bytes=%u\n",unsigned(uxTaskGetStackHighWaterMark(nullptr)));
    Serial.flush();
#if VENTANAL_PWRENTEST
    // Input sensing allows a digital level read while the output is held low.
    ESP_ERROR_CHECK(gpio_set_level(GPIO_NUM_21, 0));
    ESP_ERROR_CHECK(gpio_set_direction(GPIO_NUM_21, GPIO_MODE_INPUT_OUTPUT));
    ESP_ERROR_CHECK(gpio_hold_en(GPIO_NUM_21));
    gpio_deep_sleep_hold_en();
#endif
    esp_deep_sleep_start();
}
#if VENTANAL_STALLTEST
namespace {
// Persist the attempted index before panel access; resets advance to the next
// sample. A separate marker records an intentional timer-sleep boundary.
void stallRun(Preferences& prefs, const State& state, int pct, bool offline) {
    uint8_t attempted=0, slept=0;
    prefs.getBytes("stall-i", &attempted, 1);
    prefs.getBytes("stall-sleep", &slept, 1);
    const unsigned samples=VENTANAL_PONRETRYTEST ? 20 : VENTANAL_BOOTTIMETEST ? (VENTANAL_BOOT_CONFIRM_MS ? 20 : 32) : 40;
    for(unsigned i=unsigned(attempted)+1; i<=samples; ++i) {
#if VENTANAL_BOOTTIMETEST || VENTANAL_PONRETRYTEST
        // Also sleep after watchdog recovery: every numbered sample must be
        // a short timer boot, including the first sample of a fresh run.
        if(!slept) {
            uint8_t yes=1;
            if(prefs.putBytes("stall-sleep",&yes,1)!=1) {
                Serial.println("stalltest error=nvs-write"); sleepFor(0,state); return;
            }
            sleepFor(10,state); return;
        }
#else
        if(!slept) delay(5000);
#endif
        // One full deadline per attempt; never feed inside the driver/wait.
        ESP_ERROR_CHECK(esp_task_wdt_reset_user(wakeWatchdog));
        const bool art=i%2;
        const auto& selected=art?selectedArt(state):state.weather;
        Serial.printf("stalltest i=%u screen=%s after=%s\n",i,art?"art":"weather",slept?"deepsleep":"pause");
        uint8_t current=i, no=0;
        if(prefs.putBytes("stall-i",&current,1)!=1 || prefs.putBytes("stall-sleep",&no,1)!=1) {
            Serial.println("stalltest error=nvs-write"); sleepFor(0,state); return;
        }
        slept=0;
        if(!blitFrame(art?selectedArtPath(state):"/weather.vnf",display)) {
            Serial.println("stalltest error=cache"); sleepFor(0,state); return;
        }
        State sampleState=state;
        sampleState.active=art?Screen::Art:Screen::Weather;
        rememberPanel(sampleState,pct,offline,time(nullptr),0,i);
        drawReadout(display,selected.readout,pct,offline,isLow(pct),pendingPanel.drawnAt,VENTANAL_CLOCK_24H);
#if VENTANAL_BOOTTIMETEST
        const uint32_t gate=VENTANAL_BOOT_CONFIRM_MS ? VENTANAL_BOOT_CONFIRM_MS : 4000U+1500U*((i-1)/8);
        while(millis()<gate) delay(1);
        Serial.printf("boot_gate ms=%lu entry_ms=%lu\n",static_cast<unsigned long>(gate),static_cast<unsigned long>(millis()));
#endif
        refreshPanel();
        clearWatchdogCount(prefs);
        Serial.printf("stalltest complete i=%u\n",i);
        // The next sleep sample begins after a real ten-second timer sleep.
        if(i<samples && (VENTANAL_BOOTTIMETEST || VENTANAL_PONRETRYTEST || (i+1)%(VENTANAL_PWRENTEST ? 2 : 5)==0)) {
            uint8_t yes=1;
            if(prefs.putBytes("stall-sleep",&yes,1)!=1) {
                Serial.println("stalltest error=nvs-write"); sleepFor(0,state); return;
            }
            sleepFor(10,state); return;
        }
    }
    Serial.printf("stalltest done=%u\n",samples);
    Serial.flush();
}
}
#endif
void setup() {
    armWakeWatchdog();
    const uint32_t wakeStarted=millis();
    bool doublePress=false, longPress=false;
    if(esp_sleep_get_wakeup_cause()==ESP_SLEEP_WAKEUP_EXT0) {
        pinMode(18,INPUT_PULLUP);
        PressClassifier classifier;
        while(!classifier.done(millis()-wakeStarted)) {
            classifier.sample(millis()-wakeStarted,digitalRead(18)==LOW);
            delay(1);
        }
        doublePress=classifier.isDouble;
        longPress=classifier.isLong;
    }
    Serial.begin(115200);
    delay(1000);
#if VENTANAL_PWRENTEST
    Serial.printf("pwr_en_on_wake=%d\n",gpio_get_level(GPIO_NUM_21));
    ESP_ERROR_CHECK(gpio_hold_dis(GPIO_NUM_21));
#endif
    const uint32_t bootStart=millis();
    const auto raw=esp_sleep_get_wakeup_cause();
    WakeCause wake=raw==ESP_SLEEP_WAKEUP_EXT0 ? WakeCause::Button :
                   raw==ESP_SLEEP_WAKEUP_TIMER ? WakeCause::Timer : WakeCause::ColdBoot;
    const auto reset=esp_reset_reason();
    bool ponRetry=pendingPanel.magic==ponMagic &&
        ponRetryBoot(reset==ESP_RST_SW,pendingPanel.attempts) &&
        pendingPanel.stateChecksum==panelChecksum();
    if(!ponRetry) { pendingPanel.magic=0; completePonRetry(pendingPanel.attempts); }
    const bool watchdog=reset==ESP_RST_TASK_WDT || reset==ESP_RST_INT_WDT || reset==ESP_RST_WDT;
    wake=wakeAfterReset(wake,watchdog);
    Preferences prefs;
    const bool persisted=prefs.begin("ventanal",false);
    loadQuiet(prefs);
    loadBattery(prefs);
    watchdogResets=0;
    if(persisted) prefs.getBytes("wdt-count", &watchdogResets, sizeof watchdogResets);
    const bool skip=watchdogLimit(watchdog,watchdogResets);
#if VENTANAL_STALLTEST
    (void)skip;
#endif
    bool recoveryStored=persisted;
    if(watchdog) {
        Serial.printf("reset=watchdog count=%u\n",unsigned(watchdogResets));
        if(!persisted || prefs.putBytes("wdt-count", &watchdogResets, sizeof watchdogResets)!=sizeof watchdogResets) {
            // Without durable recovery state, never risk another panel hang.
            Serial.println("error=nvs-watchdog-write");
            recoveryStored=false;
        }
    }
#if VENTANAL_WAKETEST == 2 || VENTANAL_SPARESTEST
    if(wake==WakeCause::ColdBoot) testWakeCount=0;
    if(wake==WakeCause::Timer && ++testWakeCount==2) {
        wake=WakeCause::Button;
#if VENTANAL_SPARESTEST
        doublePress=true;
#endif
        Serial.println("diagnostic=simulated-button (physical wake=timer)");
    }
#endif
#if VENTANAL_STATELOSTTEST
    uint8_t step=0;
    prefs.getBytes("lost-step",&step,1);
    if(wake==WakeCause::ColdBoot && !ponRetry) step=0;
    if(!ponRetry && wake==WakeCause::Timer && step<3) {
        ++step; prefs.putBytes("lost-step",&step,1);
        wake=WakeCause::Button; doublePress=true;
        statelostForce=step==1;
        Serial.printf("diagnostic=statelost-double step=%u\n",unsigned(step));
    } else if(!ponRetry && wake==WakeCause::ColdBoot) prefs.putBytes("lost-step",&step,1);
    else if(!ponRetry && step==3) Serial.println("diagnostic=statelost-report-timer");
#endif
#if VENTANAL_LONGPRESSTEST
    if(wake==WakeCause::Timer && !ponRetry) {
        wake=WakeCause::Button; longPress=true; doublePress=false;
        Serial.println("diagnostic=simulated-long (physical wake=timer)");
    }
#endif
    Serial.printf("wake=%s\n",wake==WakeCause::Button?"button":wake==WakeCause::Timer?"timer":"cold");
    if(wake==WakeCause::Button) Serial.printf("press=%s\n",longPress?"long":doublePress?"double":"single");
    setenv("TZ",VENTANAL_TZ,1); tzset();
    static const State defaults={Screen::Weather,{{},{},{1020,0,580,104,false}},
                 {{},{},{1236,30,334,136,true}},{{},{},0,false}};
    State state=defaults;
    size_t stateLength=0, stateGot=0;
    bool stateLoaded=persisted;
    if(persisted && prefs.isKey("state13")) {
        stateLength=prefs.getBytesLength("state13");
        struct LegacyState { Screen active; ScreenInfo art,weather; Drawn drawn; } old{};
        struct PreviousState { Screen active; ScreenInfo art,weather; Drawn drawn; OwnerState owner{}; } previous{};
        static_assert(sizeof(PreviousState)==1084,"production State migration ABI");
        if(stateLength==sizeof previous) {
            stateGot=prefs.getBytes("state13",&previous,sizeof previous);
            if(stateGot!=sizeof previous) stateGot=prefs.getBytes("state13",&previous,sizeof previous);
            stateLoaded=stateGot==sizeof previous;
            if(stateLoaded) {
                state.active=previous.active; state.art=previous.art; state.weather=previous.weather;
                state.drawn=previous.drawn; state.owner=previous.owner;
                Serial.println("state migrated legacy-size=1084 owner=preserved");
            }
        } else if(stateLength==sizeof(SolarState)) {
            SolarState prior{};
            stateGot=prefs.getBytes("state13",&prior,sizeof prior);
            if(stateGot!=sizeof prior) stateGot=prefs.getBytes("state13",&prior,sizeof prior);
            stateLoaded=stateGot==sizeof prior;
            if(stateLoaded) {
                state.active=prior.active; state.art=prior.art; state.weather=prior.weather;
                state.drawn=prior.drawn; state.owner=prior.owner; state.solar=prior.solar;
                strcpy(state.solarDrawn,prior.solarDrawn);
                if(prior.solar.id[0]) { state.order[2]=Screen::Solar; state.orderCount=3; }
                Serial.println("state migrated solar owner=preserved");
            }
        } else if(stateLength==sizeof state) {
            stateGot=prefs.getBytes("state13",&state,sizeof state);
            if(stateGot!=sizeof state) stateGot=prefs.getBytes("state13",&state,sizeof state);
            stateLoaded=stateGot==sizeof state;
        } else if(stateLength==sizeof old) {
            stateGot=prefs.getBytes("state13",&old,sizeof old);
            if(stateGot!=sizeof old) stateGot=prefs.getBytes("state13",&old,sizeof old);
            stateLoaded=stateGot==sizeof old;
            if(stateLoaded) {
                state.active=old.active; state.art=old.art; state.weather=old.weather; state.drawn=old.drawn;
            }
        } else stateLoaded=false;
    }
    logState("boot-load",state,stateLength,stateGot);
    if(!persisted) Serial.println("error=state-load");
    if(persisted && !stateLoaded) {
        pendingPanel.magic=0;
        if(!quarantineState(stateLength)) {
            // Never replace the only surviving copy if raw recovery/storage fails.
            Serial.println("error=state-load quarantine-failed");
            prefs.end(); sleepFor(60,defaults); return;
        }
        state=defaults;
        ponRetry=false;
        wake=WakeCause::ColdBoot; // Provision online even if this was a button wake.
    }
    if(
#if !VENTANAL_STALLTEST
       skip ||
#endif
       (watchdog && !recoveryStored)) {
        Serial.println("draw skipped=watchdog-limit");
        // Initialize only the I2C bus, then seed time from the standalone RTC.
        // No panel initialization, power-up, battery read or drawing occurs.
#ifndef VENTANAL_HOST_TEST
        inkplateMutexInit();
#endif
        Wire.begin();
        syncTime();
        if(persisted) prefs.end();
        const int pct=batteryCount ? batteryPercent(correctBattery(batteryHistory[batteryCount-1])) : 100;
        sleepFor(secondsUntilNextWake(time(nullptr),VENTANAL_WAKE_MINUTES,
                 quietStart(VENTANAL_QUIET_START),quietEnd(VENTANAL_QUIET_END),isLow(pct)),state);
        return;
    }
    if(ponRetry) {
        Serial.printf("panel pon-retry attempt=%u\n",unsigned(pendingPanel.attempts));
        display.begin();
        display.setRotation(VENTANAL_ROTATION);
        const bool mounted=LittleFS.begin(false,"/littlefs",10,"littlefs");
        state=panelState();
        logState("retry-load",state);
        const auto& selected=selectedScreen(state);
        bool ready=false;
        if(pendingPanel.kind==2) drawChargeMe(display);
        else if(pendingPanel.kind==1) drawWaiting(display,VENTANAL_HOST_URL);
        else {
            ready=mounted && blitFrame(selectedPath(state),display);
            if(!ready) {
                Serial.println("panel pon-retry error=cache");
                pendingPanel.magic=0;
                pinMode(21,OUTPUT); digitalWrite(21,LOW);
                syncTime();
                sleepFor(secondsUntilNextWake(time(nullptr),VENTANAL_WAKE_MINUTES,
                    quietStart(VENTANAL_QUIET_START),quietEnd(VENTANAL_QUIET_END),isLow(pendingPanel.pct)),state);
                return;
            }
            drawReadout(display,selected.readout,pendingPanel.pct,pendingPanel.offline,
                        isLow(pendingPanel.pct),pendingPanel.drawnAt,VENTANAL_CLOCK_24H);
        }
        refreshPanel();
        clearWatchdogCount(prefs);
        Serial.println("panel pon-retry complete");
        state.drawn.any=ready;
        if(ready) {
            strcpy(state.drawn.artId,selectedArt(state).id);
            strcpy(state.drawn.weatherId,state.weather.id);
            strcpy(state.solarDrawn,state.solar.id);
            for(unsigned i=0;i<2;++i) strcpy(state.extraDrawn[i],state.extraScreens[i].id);
            state.drawn.pct=static_cast<int8_t>(pendingPanel.pct);
        }
        if(persisted) save(prefs,state);
        syncTime(); // RTC only; Wi-Fi has not been started on this boot.
#if VENTANAL_PONRETRYTEST
        if(pendingPanel.sample) {
            Serial.printf("stalltest complete i=%u\n",unsigned(pendingPanel.sample));
            if(pendingPanel.sample<20) {
                uint8_t yes=1;
                if(prefs.putBytes("stall-sleep",&yes,1)!=1) Serial.println("stalltest error=nvs-write");
                prefs.end(); if(mounted) LittleFS.end(); sleepFor(10,state); return;
            }
            Serial.println("stalltest done=20");
            prefs.end(); if(mounted) LittleFS.end(); sleepFor(3600,state); return;
        }
        stallRun(prefs,state,pendingPanel.pct,pendingPanel.offline);
#endif
        if(persisted) prefs.end();
        if(mounted) LittleFS.end();
        sleepFor(pendingPanel.kind==2 ? 0 : secondsUntilNextWake(time(nullptr),
            VENTANAL_WAKE_MINUTES,quietStart(VENTANAL_QUIET_START),quietEnd(VENTANAL_QUIET_END),isLow(pendingPanel.pct)),state);
        return;
    }
    display.begin();
    display.setRotation(VENTANAL_ROTATION);
    const float rawVolts=smoothVolts(batteryHistory,batteryCount,display.readBattery());
    const int batteryMv=static_cast<int>(std::lround(rawVolts*1000));
    const float volts=correctBattery(rawVolts);
    int pct=batteryPercent(volts);
    Serial.printf("battery=%.3f pct=%d bat_mv=%d\n",rawVolts,pct,batteryMv);
    if(isCritical(volts)) {
        rememberPanel(state,pct,false,time(nullptr),2);
        drawChargeMe(display);
        refreshPanel();
        clearWatchdogCount(prefs);
        state.drawn.any=false;
        if(persisted) { save(prefs,state); prefs.end(); }
        Serial.println("draw=charge-me timer=disabled");
        sleepFor(0,state);
        return;
    }
    if(display.width()!=1600 || display.height()!=1200) {
        Serial.println("error=landscape-required");
        if(persisted) prefs.end();
        sleepFor(60,state); return;
    }
    const bool mounted=LittleFS.begin(false,"/littlefs",10,"littlefs");
#if VENTANAL_ERASE_CACHE
    if(mounted) {
        LittleFS.remove("/art.vnf"); LittleFS.remove("/weather.vnf"); LittleFS.remove("/solar.vnf");
        LittleFS.remove("/art.vnf.tmp"); LittleFS.remove("/weather.vnf.tmp");
        Serial.println("diagnostic=cache-erased");
    }
#endif
#if VENTANAL_STALLTEST
    uint8_t attempted=0;
    if(!persisted || !mounted) { Serial.println("stalltest error=storage"); sleepFor(0,state); return; }
    uint32_t run=0;
    prefs.getBytes("stall-run", &run, sizeof run);
    if(run!=VENTANAL_STALLTEST) {
        run=VENTANAL_STALLTEST;
        uint8_t zero=0;
        if(prefs.putBytes("stall-run",&run,sizeof run)!=sizeof run ||
           prefs.putBytes("stall-i",&zero,1)!=1 || prefs.putBytes("stall-sleep",&zero,1)!=1) {
            Serial.println("stalltest error=nvs-write"); sleepFor(0,state); return;
        }
    }
    prefs.getBytes("stall-i", &attempted, 1);
    bool diagnosticReady=attempted!=0;
#if VENTANAL_BOOTTIMETEST || VENTANAL_PONRETRYTEST
    uint8_t pendingSleep=0;
    prefs.getBytes("stall-sleep", &pendingSleep, 1);
    diagnosticReady=diagnosticReady || pendingSleep;
#endif
    if(diagnosticReady) {
        syncTime();
        stallRun(prefs,state,pct,false);
        prefs.end(); LittleFS.end(); sleepFor(3600,state); return;
    }
#endif
    bool offline=false;
    uint32_t start=millis();
    if(wake!=WakeCause::Button || longPress) {
        const bool online=wifiConnect();
        Serial.printf("wifi ms=%lu\n",static_cast<unsigned long>(millis()-start));
        start=millis(); syncTime();
        Serial.printf("time ms=%lu\n",static_cast<unsigned long>(millis()-start));
        start=millis();
        ScreenInfo art{},weather{};
        // Report the frame currently on the panel, before any downloads/draw.
        setBoardReport(pct,screenName(state),
                       state.drawn.any?selectedDrawn(state):"",batteryMv);
        state.owner.queue.query(manifestSkipQuery,sizeof manifestSkipQuery);
        offline=!(online && fetchManifest(art,weather));
        if(!offline) {
            if(persisted && !storeBattery(prefs,manifestBatteryFullMv)) Serial.println("error=battery-calibration-save");
            if(!persisted) batteryFullMv=manifestBatteryFullMv;
            pct=batteryPercent(correctBattery(rawVolts));
            if(isCritical(correctBattery(rawVolts))) {
                rememberPanel(state,pct,false,time(nullptr),2);
                drawChargeMe(display); refreshPanel(); clearWatchdogCount(prefs);
                state.drawn.any=false;
                if(persisted) { save(prefs,state); prefs.end(); }
                sleepFor(0,state); return;
            }
            if(state.owner.queue.count) Serial.printf("skip reports acknowledged count=%u\n",unsigned(state.owner.queue.count));
            state.owner.queue.acknowledge(true);
            state.owner.mainChanged(art.id);
            if(persisted) save(prefs,state);
        }
        Serial.printf("manifest ok=%d ms=%lu\n",!offline,static_cast<unsigned long>(millis()-start));
        start=millis();
        if(!offline && persisted && quietHours.valid()) storeQuiet(prefs,quietHours);
        if(!offline && mounted) {
            // All eight slots share an absolute wake deadline, measured from
            // watchdog arming. Never feed the watchdog, including during display.
            uint8_t next=0;
            if(persisted) prefs.getBytes("dl-next",&next,sizeof next);
            if(next>=8) next=0;
            for(unsigned step=0;step<8;++step) {
                const unsigned slot=(next+step)%8;
                ScreenInfo* cached=nullptr;
                const ScreenInfo* incoming=nullptr;
                char path[24];
                if(slot==0) { cached=&state.art; incoming=&art; strcpy(path,"/art.vnf"); }
                else if(slot==1) { cached=&state.weather; incoming=&weather; strcpy(path,"/weather.vnf"); }
                else if(slot==2) { cached=&state.solar; incoming=&manifestExtras.solar; strcpy(path,"/solar.vnf"); }
                else if(slot<5) {
                    const unsigned i=slot-3;
                    cached=&state.extraScreens[i]; incoming=&manifestExtras.extraScreens[i];
                    snprintf(path,sizeof path,"/extra%u.vnf",i);
                    if(strcmp(state.extraNames[i],manifestExtras.names[i])) {
                        LittleFS.remove(path); *cached={}; state.extraDrawn[i][0]=0;
                        if(state.active==static_cast<Screen>(3+i)) { state.active=Screen::Art; state.drawn.any=false; }
                    }
                    strcpy(state.extraNames[i],manifestExtras.names[i]);
                } else {
                    const unsigned i=slot-5;
                    cached=&state.owner.spares[i]; incoming=&manifestExtras.spares[i];
                    snprintf(path,sizeof path,"/spare%u.vnf",i);
                    if(i>=manifestExtras.count) incoming=nullptr;
                }
                if(!incoming || !incoming->id[0]) {
                    LittleFS.remove(path); LittleFS.remove((String(path)+".tmp").c_str()); *cached={};
                    if(slot==2) {
                        state.solarDrawn[0]=0;
                        if(state.active==Screen::Solar) { state.active=Screen::Art; state.drawn.any=false; }
                    } else if(slot>=3 && slot<5) {
                        state.extraDrawn[slot-3][0]=0;
                        if(state.active==static_cast<Screen>(slot)) { state.active=Screen::Art; state.drawn.any=false; }
                    }
                    continue;
                }
                if(!strcmp(cached->id,incoming->id) && LittleFS.exists(path)) continue;
                if(!canDownloadSpare(millis()-wakeStarted)) {
                    uint8_t deferred=slot;
                    if(persisted) prefs.putBytes("dl-next",&deferred,sizeof deferred);
                    Serial.printf("download deferred slot=%u wake-budget\n",slot);
                    break;
                }
                // Persist progress before a transfer: a timeout/failure cannot
                // monopolize subsequent wakes. The first deferred slot wins.
                uint8_t following=(slot+1)%8;
                if(persisted) prefs.putBytes("dl-next",&following,sizeof following);
                if(slot>=3 && slot<5 && LittleFS.totalBytes()<LittleFS.usedBytes()+960016+65536) {
                    Serial.println("extra download deferred=filesystem-budget"); continue;
                }
                update(prefs,state,*cached,*incoming,path);
            }
            state.orderCount=manifestExtras.orderCount;
            for(unsigned i=0;i<state.orderCount;++i) state.order[i]=manifestExtras.order[i];
            if(!state.orderCount) { // Legacy host fixtures and old manifests.
                state.order[0]=Screen::Art; state.order[1]=Screen::Weather; state.orderCount=2;
                if(state.solar.id[0]) { state.order[2]=Screen::Solar; state.orderCount=3; }
            }
            Serial.printf("screens order=");
            for(unsigned i=0;i<state.orderCount;++i) { State item=state; item.active=state.order[i]; Serial.printf("%s%s",i?",":"",screenName(item)); }
            Serial.println("");
        }
        Serial.printf("download ms=%lu\n",static_cast<unsigned long>(millis()-start));
    } else {
        // RTC seeds local time without starting Wi-Fi or SNTP.
        syncTime();
        Serial.println("wifi skipped=button");
    }
    if(wake==WakeCause::Button && !longPress && doublePress && mounted && persisted) {
        bool cached[3];
        char path[24];
        for(int i=0;i<3;i++) {
            snprintf(path,sizeof path,"/spare%d.vnf",i);
            cached[i]=state.owner.spares[i].id[0] && LittleFS.exists(path);
        }
        const int next=nextSpare(state.owner.cursor,cached);
        char skipped[17]; strcpy(skipped,selectedArt(state).id);
        bool ready=true;
        if(next>=0) {
            snprintf(path,sizeof path,"/spare%d.vnf",next);
            ready=copyOverride(path);
        }
        if(ready) {
            state.owner.cursor=next;
            state.owner.overrideArt=next<0?ScreenInfo{}:state.owner.spares[next];
            state.owner.queue.push(skipped,selectedArt(state).id);
            state.active=Screen::Art;
            save(prefs,state); // Selection and feedback survive a PON restart.
            Serial.printf("spare selected=%d id=%s queued=%u\n",next,selectedArt(state).id,unsigned(state.owner.queue.count));
        }
    } else if(!longPress) state.active=cycleScreen(state,wake,mounted);
    if(static_cast<unsigned>(state.active)>=2 && (!mounted || !screenCached(state,state.active))) state.active=Screen::Art;
    const ScreenInfo& selected=selectedScreen(state);
    const char* path=selectedPath(state);
    const bool cached=mounted && LittleFS.exists(path);
    const bool redraw=longPress || (static_cast<unsigned>(state.active)>=2 ? wake!=WakeCause::Timer || !state.drawn.any || strcmp(selectedDrawn(state),selected.id)!=0 : needsRedraw(wake,state.active,state.drawn,selectedArt(state).id,state.weather.id,pct));
    start=millis();
    if(!cached || redraw) {
        const bool ready=cached && blitFrame(path,display);
        Serial.printf("blit ms=%lu valid=%d\n",static_cast<unsigned long>(millis()-start),ready);
        if(ready) {
            const time_t drawnAt=time(nullptr);
            rememberPanel(state,pct,offline,drawnAt);
            const uint32_t overlayStart=millis();
            drawReadout(display,selected.readout,pct,offline,isLow(pct),drawnAt,VENTANAL_CLOCK_24H);
            Serial.printf("readout ms=%lu\n",static_cast<unsigned long>(millis()-overlayStart));
            char local[40]; tm now{}; localtime_r(&drawnAt,&now);
            strftime(local,sizeof local,"%Y-%m-%d %H:%M:%S",&now);
            Serial.printf("draw=%s updated=%s offline=%d rect=%d,%d,%d,%d\n",screenName(state),local,offline,
                          selected.readout.x,selected.readout.y,selected.readout.w,selected.readout.h);
        } else {
            rememberPanel(state,pct,offline,time(nullptr),1);
            drawWaiting(display,VENTANAL_HOST_URL);
            Serial.println("draw=waiting-for-host");
        }
        start=millis(); refreshPanel();
        clearWatchdogCount(prefs);
        Serial.printf("display ms=%lu\n",static_cast<unsigned long>(millis()-start));
        state.drawn.any=ready;
        if(ready) {
            strcpy(state.drawn.artId,selectedArt(state).id);
            strcpy(state.drawn.weatherId,state.weather.id);
            strcpy(state.solarDrawn,state.solar.id);
            for(unsigned i=0;i<2;++i) strcpy(state.extraDrawn[i],state.extraScreens[i].id);
            state.drawn.pct=static_cast<int8_t>(pct);
        }
    } else {
        Serial.println("draw skipped=unchanged");
        clearWatchdogCount(prefs);
    }
#if VENTANAL_STALLTEST
    stallRun(prefs,state,pct,offline);
#endif
    if(persisted) { save(prefs,state); prefs.end(); }
    if(mounted) LittleFS.end();
    const uint32_t seconds=secondsUntilNextWake(time(nullptr),VENTANAL_WAKE_MINUTES,
                              quietStart(VENTANAL_QUIET_START),quietEnd(VENTANAL_QUIET_END),isLow(pct));
#if VENTANAL_STATELOSTTEST
    if(wake==WakeCause::Timer) Serial.println("diagnostic=statelost-done");
#endif
    Serial.printf("wake total ms=%lu\n",static_cast<unsigned long>(millis()-bootStart));
#if VENTANAL_WAKETEST || VENTANAL_SPARESTEST || VENTANAL_LONGPRESSTEST
    (void)seconds;
    sleepFor(60,state);
#else
    sleepFor(seconds,state);
#endif
}
void loop() {}
