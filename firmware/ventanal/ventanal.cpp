// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
#include <Arduino.h>
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
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <Wire.h>
#ifndef VENTANAL_HOST_TEST
#include <system/inkplateSemaphore.h>
#endif
#include <esp_rtc_time.h>
#include "sd_card.h"
#include "../common/resume/storage.h"
#include <driver/rtc_io.h>
#include <driver/gpio.h>
#include "settings.h"
#ifndef VENTANAL_HOST_TEST
// TLS certificate rejection uses more stack than a successful connection.
// Reserve margin for mbedTLS verification and the existing wake State.
SET_LOOP_TASK_STACK_SIZE(16384);
#endif
#include "net.h"
#include "quiet.h"
#include "owner.h"
#include <algorithm>
#include <memory>
#include <new>
#include <type_traits>
#include "cache.h"
#include "security.h"
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
// Retain the existing wdt-count NVS key so upgrades keep their strikes.
uint8_t watchdogResets=0;
bool otaPendingBoot=false;
namespace {
bool isWatchdogReset(esp_reset_reason_t reason) {
    return reason==ESP_RST_TASK_WDT || reason==ESP_RST_INT_WDT || reason==ESP_RST_WDT;
}
bool isAbnormalReset(esp_reset_reason_t reason) {
    switch(reason) {
        case ESP_RST_POWERON: case ESP_RST_EXT: case ESP_RST_SW:
        case ESP_RST_DEEPSLEEP: case ESP_RST_SDIO: case ESP_RST_USB: case ESP_RST_JTAG:
            return false;
        default:
            return true;
    }
}
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
RTC_DATA_ATTR ventanal::ContentStaleness contentStaleness{};
RTC_DATA_ATTR bool downloadFailureDrawn = false;
RTC_DATA_ATTR uint64_t sdProbeRetryAtUs = 0;
bool sdProbeAttempted = false;

void prepareDownloadStorage() {
    constexpr uint64_t retryDelayUs = 24ULL * 60 * 60 * 1000000;
    // The RTC counter includes deep sleep and is independent of SNTP.
    if (sdProbeAttempted || esp_rtc_get_time_us() < sdProbeRetryAtUs) return;
    sdProbeAttempted = true;
    const bool mounted = mountSdCard();
    sdProbeRetryAtUs = mounted ? 0 : esp_rtc_get_time_us() + retryDelayUs;
    selectPartialStore(mounted);
#ifndef VENTANAL_HOST_TEST
    if (mounted) sweepPartials(ventanal::trustedEpoch(time(nullptr)), [](const PartialMeta&) { return true; });
#endif
}
#if VENTANAL_WAKETEST == 2 || VENTANAL_SPARESTEST
RTC_DATA_ATTR uint8_t testWakeCount = 0;
#endif

void sleepHardware(uint32_t seconds, bool allowWake=true);
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
void sleepFor(uint32_t seconds, const State& state, bool allowWake=true);
void logState(const char* phase, const State& state, size_t length=0, size_t got=0) {
    Serial.printf("state phase=%s size=%u length=%u got=%u queue=%u cursor=%d override=%.16s spares=%.16s,%.16s,%.16s\n",
        phase,unsigned(sizeof state),unsigned(length),unsigned(got),unsigned(state.owner.queue.count),
        state.owner.cursor,state.owner.overrideArt.id,state.owner.spares[0].id,
        state.owner.spares[1].id,state.owner.spares[2].id);
}
void sleepFor(uint32_t seconds, const State& state, bool allowWake) {
    logState("sleep",state);
    sleepHardware(seconds,allowWake);
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
bool canProvisionCache() {
    // A missing namespace cannot contain either state key.
    nvs_handle_t handle;
    const auto err=nvs_open("ventanal",NVS_READONLY,&handle);
    if(err==ESP_ERR_NVS_NOT_FOUND) return true;
    if(err!=ESP_OK) return false;
    const bool fresh=nvs_find_key(handle,"state13",nullptr)==ESP_ERR_NVS_NOT_FOUND &&
                     nvs_find_key(handle,"state-bad",nullptr)==ESP_ERR_NVS_NOT_FOUND;
    nvs_close(handle);
    return fresh;
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
char* drawnId(State& state, Screen screen) {
    if(screen==Screen::Extra0 || screen==Screen::Extra1) return state.extraDrawn[static_cast<unsigned>(screen)-3];
    return screen==Screen::Art?state.drawn.artId:screen==Screen::Solar?state.solarDrawn:state.drawn.weatherId;
}
// Never "waiting for host" while a complete screen is cached: when the
// scheduled screen is missing, show weather, else art, else solar or an
// extra. The scheduled screen stays active, so it downloads first.
constexpr Screen fallbackScreens[]={Screen::Weather,Screen::Art,Screen::Solar,Screen::Extra0,Screen::Extra1};
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
    bool downloadFailure;
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
constexpr uint32_t panelRecoveryStackBytes=8192; // ESP-IDF task stack sizes are in bytes.
portMUX_TYPE panelLock=portMUX_INITIALIZER_UNLOCKED;
TaskHandle_t panelRecoveryTask=nullptr;
esp_timer_handle_t panelTimer=nullptr;
uint32_t panelStarted;
#if VENTANAL_PONRETRYTEST
uint32_t panelFirstHigh;
#endif
bool panelSeenHigh;
bool panelMonitoring;
bool panelRecoveryPending;
void rememberPanel(const State& state, int pct, bool offline, time_t drawnAt,
                   uint8_t kind=0, uint8_t sample=0, bool downloadFailure=false) {
    pendingPanel.magic=0; pendingPanel.attempts=0;
    pendingPanel.kind=kind; pendingPanel.offline=offline;
    pendingPanel.downloadFailure=downloadFailure;
    pendingPanel.pct=pct; pendingPanel.drawnAt=drawnAt; pendingPanel.sample=sample;
    memcpy(pendingPanel.stateBytes,&state,sizeof state);
    pendingPanel.stateChecksum=panelChecksum();
}
void checkPanel(void*) {
    // Never reconfigure BUSYN: the library owns its INPUT_PULLUP mode.
    bool notify=false;
    portENTER_CRITICAL(&panelLock);
    if(panelMonitoring && !panelSeenHigh) {
        const uint32_t elapsed=uint32_t(millis()-panelStarted);
        // Ignore power-up/reset pulses; only the post-reset PON window counts.
        if(!statelostForce && elapsed>=500 && elapsed<=3000 && digitalRead(7)==HIGH) {
            panelSeenHigh=true;
#if VENTANAL_PONRETRYTEST
            panelFirstHigh=elapsed;
#endif
        }
        if(ponStalled(elapsed,panelSeenHigh)) {
            panelMonitoring=false;
            panelRecoveryPending=true;
            notify=true;
        }
    }
    portEXIT_CRITICAL(&panelLock);
    if(notify) xTaskNotifyGive(panelRecoveryTask);
}
void recoverPanel(void*) {
    ulTaskNotifyTake(pdTRUE,portMAX_DELAY);
    ESP_ERROR_CHECK(esp_timer_stop(panelTimer));
    if(pendingPanel.kind==2) {
        pendingPanel.magic=0;
        Serial.println("panel pon-stall charge-me");
        sleepFor(0,panelState(),false);
    } else if(!nextPonRetry(pendingPanel.attempts)) {
        pendingPanel.magic=0;
        Serial.println("panel pon-retry limit");
        // Do not call the driver's powerOff(), which waits on stuck BUSYN.
        pinMode(21,OUTPUT); digitalWrite(21,LOW);
        sleepFor(secondsUntilNextWake(time(nullptr),installation.wakeMinutes,
            quietStart(installation.quietStart),quietEnd(installation.quietEnd),isLow(pendingPanel.pct)),panelState());
    } else {
        pendingPanel.magic=ponMagic;
        Serial.printf("panel pon-stall restart attempt=%u\n",unsigned(pendingPanel.attempts));
        Serial.flush();
        esp_restart();
    }
}
void refreshPanel() {
#if VENTANAL_HANGTEST
    Serial.println("diagnostic=hang-before-display");
    Serial.flush();
    while(true) delay(1);
#endif
    if(!panelTimer) {
        esp_timer_create_args_t args={};
        args.callback=checkPanel;
        args.name="panel-pon";
        ESP_ERROR_CHECK(esp_timer_create(&args,&panelTimer));
        // Keep one worker per boot, including diagnostic runs with many draws.
        ESP_ERROR_CHECK(xTaskCreate(recoverPanel,"panel-recovery",panelRecoveryStackBytes,nullptr,
                                   uxTaskPriorityGet(nullptr)+1,&panelRecoveryTask)==pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
    }
    portENTER_CRITICAL(&panelLock);
    panelSeenHigh=false;
#if VENTANAL_PONRETRYTEST
    panelFirstHigh=0;
#endif
    panelRecoveryPending=false;
    panelMonitoring=true;
    panelStarted=millis();
    portEXIT_CRITICAL(&panelLock);
    ESP_ERROR_CHECK(esp_timer_start_periodic(panelTimer,50000));
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
    portENTER_CRITICAL(&panelLock);
    panelMonitoring=false;
    const bool recovering=panelRecoveryPending;
#if VENTANAL_PONRETRYTEST
    const uint32_t firstHigh=panelFirstHigh;
#endif
    portEXIT_CRITICAL(&panelLock);
    // A claimed stall owns the snapshot and final outcome, even if the driver returns.
    if(recovering) for(;;) delay(1);
    ESP_ERROR_CHECK(esp_timer_stop(panelTimer));
#if VENTANAL_PONRETRYTEST
    if(firstHigh) Serial.printf("panel busy-high ms=%lu\n",static_cast<unsigned long>(firstHigh));
#endif
    pendingPanel.magic=0;
    completePonRetry(pendingPanel.attempts);
}
void save(Preferences& prefs, const State& state) {
    logState("save",state);
    if(prefs.putBytes("state13", &state, sizeof state) != sizeof state)
        Serial.println("error=nvs-state-write");
}
constexpr char chargeMarker[]="charge-drawn";
bool storeChargeMarker(Preferences& prefs, uint8_t value) {
    if(prefs.putBytes(chargeMarker,&value,sizeof value)==sizeof value) return true;
    Serial.println("error=nvs-charge-marker-write");
    return false;
}
void sleepCritical(Preferences& prefs, const State& state, int pct, bool chargeAttempted) {
    // Commit before the high-current refresh. An interrupted draw can leave
    // an incomplete screen, but must not repeat on the depleted battery.
    if(!chargeAttempted) {
        storeChargeMarker(prefs,1);
        rememberPanel(state,pct,false,time(nullptr),2);
        drawChargeMe(display);
        refreshPanel();
        clearWatchdogCount(prefs);
        Serial.println("draw=charge-me wake=disabled");
    } else {
        Serial.println("draw skipped=charge-me wake=disabled");
    }
    prefs.end();
    sleepFor(0,state,false);
}
}
void sleepHardware(uint32_t seconds, bool allowWake) {
    unmountSdCard();
#if VENTANAL_STATELOSTTEST
    if(seconds) seconds=10;
#endif
#if VENTANAL_SPARESTEST || VENTANAL_LONGPRESSTEST
    if(seconds) seconds=60;
#endif
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    if(allowWake) {
        rtc_gpio_pullup_en(GPIO_NUM_18);
        rtc_gpio_pulldown_dis(GPIO_NUM_18);
        esp_sleep_enable_ext0_wakeup(GPIO_NUM_18, 0);
        if(seconds) esp_sleep_enable_timer_wakeup(uint64_t(seconds)*1000000ULL);
        Serial.printf("sleep seconds=%lu button=GPIO18/LOW\n",static_cast<unsigned long>(seconds));
    } else {
        ESP_ERROR_CHECK(esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL));
        Serial.println("sleep wake=disabled");
    }
    if(!allowWake || VENTANAL_PWRENTEST) {
        // SW_C controls the panel rail. Cut it directly even if BUSYN is stuck.
        ESP_ERROR_CHECK(gpio_set_level(GPIO_NUM_21, 0));
        pinMode(21,OUTPUT);
        ESP_ERROR_CHECK(gpio_set_direction(GPIO_NUM_21, GPIO_MODE_INPUT_OUTPUT));
        ESP_ERROR_CHECK(gpio_hold_en(GPIO_NUM_21));
        gpio_deep_sleep_hold_en();
    }
    if(!allowWake) {
        // Float the unpowered panel's reset, mode, busy, data and SPI pins.
        constexpr int panelPins[]={4,5,6,7,14,38,39,40,41,42};
        for(int pin : panelPins) pinMode(pin,INPUT);
    }
    Serial.printf("stack min-free bytes=%u\n",unsigned(uxTaskGetStackHighWaterMark(nullptr)));
    Serial.flush();
    if(otaPendingBoot) rollbackOtaBoot();
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
        drawReadout(display,selected.readout,pct,offline,isLow(pct),pendingPanel.drawnAt,installation.clock24h);
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
    if(VENTANAL_TEST_RELEASE_KEY) Serial.println("!!! TEST RELEASE KEY - BENCH FIRMWARE ONLY !!!");
#if VENTANAL_PWRENTEST
    Serial.printf("pwr_en_on_wake=%d\n",gpio_get_level(GPIO_NUM_21));
#endif
    ESP_ERROR_CHECK(gpio_hold_dis(GPIO_NUM_21));
    gpio_deep_sleep_hold_dis();
    const uint32_t bootStart=millis();
    const auto raw=esp_sleep_get_wakeup_cause();
    WakeCause wake=raw==ESP_SLEEP_WAKEUP_EXT0 ? WakeCause::Button :
                   raw==ESP_SLEEP_WAKEUP_TIMER ? WakeCause::Timer : WakeCause::ColdBoot;
    const auto reset=esp_reset_reason();
    sdProbeAttempted = false;
    if (reset == ESP_RST_POWERON) sdProbeRetryAtUs = 0;
    Preferences prefs;
    const bool persisted=prefs.begin("ventanal",false);
    if(persisted) contentStaleness.restore(prefs);
    auto otaBoot=readOtaBoot(prefs,persisted);
    otaPendingBoot=otaBoot.pending;
    const bool configured=initializeSettings(!otaPendingBoot);
    bool ponRetry=!otaBoot.rolledBack && !otaPendingBoot && pendingPanel.magic==ponMagic && pendingPanel.kind!=2 &&
        ponRetryBoot(reset==ESP_RST_SW,pendingPanel.attempts) &&
        pendingPanel.stateChecksum==panelChecksum();
    if(!ponRetry) { pendingPanel.magic=0; completePonRetry(pendingPanel.attempts); }

    const bool abnormal=isAbnormalReset(reset) && !otaBoot.rolledBack && !otaBoot.pending;
    wake=wakeAfterReset(wake,abnormal);
    watchdogResets=0;
    if(persisted) prefs.getBytes("wdt-count", &watchdogResets, sizeof watchdogResets);
    const bool skip=watchdogLimit(abnormal,watchdogResets);
#if VENTANAL_STALLTEST
    (void)skip;
#endif
    bool recoveryStored=persisted;
    if(abnormal) {
        Serial.printf("reset=%s count=%u reason=%d\n",isWatchdogReset(reset)?"watchdog":"abnormal",
                      unsigned(watchdogResets),int(reset));
        if(!persisted || prefs.putBytes("wdt-count", &watchdogResets, sizeof watchdogResets)!=sizeof watchdogResets) {
            // Without durable recovery state, never risk another panel hang.
            Serial.println("error=nvs-watchdog-write");
            recoveryStored=false;
        }
    }
    uint8_t charge=0;
    const bool chargeAttempted=persisted &&
        (prefs.getBytes(chargeMarker,&charge,sizeof charge)==sizeof charge ? charge!=0 : prefs.isKey(chargeMarker));
    if(abnormal && chargeAttempted) {
        Serial.println("draw skipped=charge-me wake=disabled");
        prefs.end();
        sleepHardware(0,false);
        return;
    }
    loadQuiet(prefs);
    loadBattery(prefs);
    if(
#if !VENTANAL_STALLTEST
       skip ||
#endif
       (abnormal && !recoveryStored)) {
        Serial.println("draw skipped=reset-limit");
        if(!configured) {
            if(persisted) prefs.end();
            sleepHardware(0);
            return;
        }
        // Seed time from the standalone RTC without initializing the panel.
#ifndef VENTANAL_HOST_TEST
        inkplateMutexInit();
#endif
        Wire.begin();
        syncTime();
        if(persisted) prefs.end();
        const int pct=batteryCount ? batteryPercent(correctBattery(batteryHistory[batteryCount-1])) : 100;
        sleepHardware(secondsUntilNextWake(time(nullptr),installation.wakeMinutes,
                      quietStart(installation.quietStart),quietEnd(installation.quietEnd),isLow(pct)));
        return;
    }
    armWakeWatchdog();
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
    setenv("TZ",installation.timezone,1); tzset();
    static const State defaults={Screen::Weather,{{},{},{1020,0,580,104,false}},
                 {{},{},{1236,30,334,136,true}},{{},{},0,false}};
    State state=defaults;
    size_t stateLength=0, stateGot=0;
    bool stateLoaded=persisted;
    // Existing installations can update their state during a mount failure.
    // Creating a record before the first successful mount would block provisioning.
    const bool hadState=persisted && prefs.isKey("state13");
    if(hadState) {
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
    // An interrupted charge draw may have left the saved frame ID unchanged.
    if(chargeAttempted) state.drawn.any=false;
    if(ponRetry) {
        Serial.printf("panel pon-retry attempt=%u\n",unsigned(pendingPanel.attempts));
        display.begin();
        display.setRotation(installation.rotation);
        const bool mounted=LittleFS.begin(false,"/littlefs",10,"littlefs");
        state=panelState();
        logState("retry-load",state);
        const auto& selected=selectedScreen(state);
        bool ready=false;
        if(pendingPanel.kind==1) drawWaiting(display,installation.host);
        else {
            ready=mounted && blitFrame(selectedPath(state),display);
            if(!ready) {
                Serial.println("panel pon-retry error=cache");
                pendingPanel.magic=0;
                pinMode(21,OUTPUT); digitalWrite(21,LOW);
                syncTime();
                sleepFor(secondsUntilNextWake(time(nullptr),installation.wakeMinutes,
                    quietStart(installation.quietStart),quietEnd(installation.quietEnd),isLow(pendingPanel.pct)),state);
                return;
            }
            drawReadout(display,selected.readout,pendingPanel.pct,pendingPanel.offline,
                        isLow(pendingPanel.pct),pendingPanel.drawnAt,installation.clock24h);
        }
        if(pendingPanel.downloadFailure) drawDownloadFailure(display);
        refreshPanel();
        clearWatchdogCount(prefs);
        if(chargeAttempted) storeChargeMarker(prefs,0);
        downloadFailureDrawn=pendingPanel.downloadFailure;
        Serial.println("panel pon-retry complete");
        state.drawn.any=ready;
        if(ready) {
            strcpy(state.drawn.artId,selectedArt(state).id);
            strcpy(state.drawn.weatherId,state.weather.id);
            strcpy(state.solarDrawn,state.solar.id);
            for(unsigned i=0;i<2;++i) strcpy(state.extraDrawn[i],state.extraScreens[i].id);
            state.drawn.pct=static_cast<int8_t>(pendingPanel.pct);
        }
        if(persisted && (mounted || hadState)) save(prefs,state);
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
        sleepFor(secondsUntilNextWake(time(nullptr),
            installation.wakeMinutes,quietStart(installation.quietStart),quietEnd(installation.quietEnd),isLow(pendingPanel.pct)),state);
        return;
    }
    display.begin();
    display.setRotation(installation.rotation);
    const float rawVolts=smoothVolts(batteryHistory,batteryCount,display.readBattery());
    const int batteryMv=static_cast<int>(std::lround(rawVolts*1000));
    const float volts=correctBattery(rawVolts);
    int pct=batteryPercent(volts);
    Serial.printf("battery=%.3f pct=%d bat_mv=%d\n",rawVolts,pct,batteryMv);
    if(isCritical(volts)) {
        sleepCritical(prefs,state,pct,chargeAttempted);
        return;
    }
    if(display.width()!=1600 || display.height()!=1200) {
        Serial.println("error=landscape-required");
        if(persisted) prefs.end();
        sleepFor(60,state); return;
    }
    if(!configured) {
        Serial.println("setup=provisioning-required");
        rememberPanel(state,pct,true,time(nullptr),1);
        drawWaiting(display,"Not set up");
        refreshPanel();
        clearWatchdogCount(prefs);
        if(persisted) prefs.end();
        sleepFor(0,state);
        return;
    }
    bool mounted=LittleFS.begin(false,"/littlefs",10,"littlefs");
    // A wired migration moves LittleFS but preserves the cached NVS state.
    // Allow initialization at the new offset once, then preserve mount failures.
    constexpr char cacheLayoutKey[]="cache-ota";
    const bool newCacheLayout=persisted && otaCacheLayout() && !prefs.isKey(cacheLayoutKey);
    if(!mounted && persisted && (newCacheLayout || canProvisionCache())) {
        Serial.println(newCacheLayout?"cache provisioning=ota-layout":"cache provisioning=mount-failed-no-state");
        mounted=LittleFS.begin(true,"/littlefs",10,"littlefs");
        if(!mounted) {
            // Keep the provisioning path available on the next wake.
            Serial.println("error=cache-provision");
            prefs.end();
            syncTime();
            sleepFor(secondsUntilNextWake(time(nullptr),installation.wakeMinutes,
                quietStart(installation.quietStart),quietEnd(installation.quietEnd),isLow(pct)),state);
            return;
        }
        wake=WakeCause::ColdBoot;
    }
    if(mounted && newCacheLayout) {
        const uint8_t initialized=1;
        if(prefs.putBytes(cacheLayoutKey,&initialized,sizeof initialized)!=sizeof initialized)
            Serial.println("error=cache-layout-save");
    }
    if(mounted && state.owner.overrideArt.id[0] && !LittleFS.exists("/override.vnf")) {
        state.owner.overrideArt={};
        state.owner.cursor=-1;
    }
    if(!mounted) Serial.println("error=cache-mount");
#ifndef VENTANAL_HOST_TEST
    if (mounted) sweepPartials(seedRtcTime(), [](const PartialMeta&) { return true; });
#endif
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
    bool contentDue=contentStaleness.due;
    bool manifestOk=false;
    uint32_t start=millis();
    if(wake!=WakeCause::Button || longPress || otaPendingBoot) {
        bool online=wifiConnect();
        Serial.printf("wifi ms=%lu\n",static_cast<unsigned long>(millis()-start));
        start=millis(); bool timeSynced=syncTime();
        Serial.printf("time ms=%lu\n",static_cast<unsigned long>(millis()-start));
        start=millis();
        ScreenInfo art{},weather{};
        // Report the frame currently on the panel, before any downloads/draw.
        setBoardReport(pct,screenName(state),
                       state.drawn.any?selectedDrawn(state):"",batteryMv,otaBoot.record,
                       otaBoot.attemptState.attempts.approval);
        state.owner.queue.query(manifestSkipQuery,sizeof manifestSkipQuery);
        bool transient=false;
        manifestOk=online && fetchManifest(art,weather,&transient);
        if(otaPendingBoot && otaBoot.matching && !manifestOk && (!online || transient)) {
            // Deep sleep resets the CPU and the bootloader rejects a pending
            // image. A timed light-sleep wake keeps the same health deadline.
            Serial.println("ota retry=host-unreachable");
            WiFi.disconnect(true);
            WiFi.mode(WIFI_OFF);
            if(esp_sleep_enable_timer_wakeup(5000000ULL)==ESP_OK) {
                esp_light_sleep_start();
                esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_TIMER);
                online=wifiConnect();
                timeSynced=syncTime();
                manifestOk=online && fetchManifest(art,weather);
            }
        }
        offline=!manifestOk;
        const ManifestFrameSlots incomingFrames{
            &art, &weather, &manifestExtras.solar,
            &manifestExtras.extraScreens[0], &manifestExtras.extraScreens[1],
            manifestExtras.count > 0 ? &manifestExtras.spares[0] : nullptr,
            manifestExtras.count > 1 ? &manifestExtras.spares[1] : nullptr,
            manifestExtras.count > 2 ? &manifestExtras.spares[2] : nullptr
        };
        const std::array<ScreenInfo*, frameCachePaths.size()> cachedFrames{
            &state.art, &state.weather, &state.solar, &state.extraScreens[0], &state.extraScreens[1],
            &state.owner.spares[0], &state.owner.spares[1], &state.owner.spares[2]
        };
        const auto acceptFrame = [&](unsigned slot) {
            if(strcmp(cachedFrames[slot]->id, incomingFrames[slot]->id) != 0)
                contentStaleness.clearOverdue();
            *cachedFrames[slot] = *incomingFrames[slot];
            if (slot >= 3 && slot < 5) strcpy(state.extraNames[slot-3], manifestExtras.names[slot-3]);
        };
        const auto recoverFrames = [&] {
            const auto recovered = sweepFramePartials(incomingFrames);
            for (unsigned slot = 0; slot < frameCachePaths.size(); ++slot)
                if (recovered & (1u << slot)) acceptFrame(slot);
            return recovered;
        };
        if(manifestOk) {
            if(persisted && manifestBoardQuery[0]) acknowledgeOtaReport(prefs,otaBoot);
            if(persisted && !storeBattery(prefs,manifestBatteryFullMv)) Serial.println("error=battery-calibration-save");
            if(!persisted) batteryFullMv=manifestBatteryFullMv;
            pct=batteryPercent(correctBattery(rawVolts));
            if(isCritical(correctBattery(rawVolts))) {
                if(mounted) LittleFS.end();
                sleepCritical(prefs,state,pct,chargeAttempted);
                return;
            }
            if(mounted) recoverFrames();
            if(state.owner.queue.count) Serial.printf("skip reports acknowledged count=%u\n",unsigned(state.owner.queue.count));
            state.owner.queue.acknowledge(true);
            state.owner.mainChanged(art.id);
            if(persisted && (mounted || hadState)) save(prefs,state);
        }
        Serial.printf("manifest ok=%d ms=%lu\n",!offline,static_cast<unsigned long>(millis()-start));
        start=millis();
        if(!offline && persisted && quietHours.valid()) storeQuiet(prefs,quietHours);
        // OTA runs first and retains its own transfer and install reserves.
        const bool otaFailed=online && persisted && mounted && millis()-wakeStarted<90000
            && tryOta(prefs,otaBoot,manifestOk,timeSynced,pct,
                      quietStart(installation.quietStart),quietEnd(installation.quietEnd),
                      prepareDownloadStorage,
                      [] { ESP_ERROR_CHECK(esp_task_wdt_reset_user(wakeWatchdog)); });
        if(otaFailed) Serial.println("download deferred=failed-ota");
        if(!offline && mounted && !otaFailed) {
            // Order: the screen on display (or scheduled) when missing or
            // stale, then weather, then the other visible screens in manifest
            // order, then spares in round-robin. The first frame fetched gets
            // the whole remaining wake budget; only spares move the cursor.
            constexpr unsigned weatherSlot=1, firstSpareSlot=5;
            constexpr unsigned spareSlots=frameCachePaths.size()-firstSpareSlot;
            std::array<uint8_t, frameCachePaths.size()> downloadOrder{};
            unsigned orderLength=0;
            const auto schedule=[&](unsigned slot) {
                for(unsigned i=0;i<orderLength;++i) if(downloadOrder[i]==slot) return;
                downloadOrder[orderLength++]=static_cast<uint8_t>(slot);
            };
            // A displayed override is a cached copy; it needs no download.
            if(state.active!=Screen::Art || !state.owner.overrideArt.id[0]) schedule(static_cast<unsigned>(state.active));
            schedule(weatherSlot);
            for(unsigned i=0;i<manifestExtras.orderCount;++i) schedule(static_cast<unsigned>(manifestExtras.order[i]));
            for(unsigned slot=0;slot<firstSpareSlot;++slot) schedule(slot);
            uint8_t next=firstSpareSlot;
            if(persisted) prefs.getBytes("dl-next",&next,sizeof next);
            if(next<firstSpareSlot || next>=frameCachePaths.size()) next=firstSpareSlot;
            for(unsigned i=0;i<spareSlots;++i) schedule(firstSpareSlot+(next-firstSpareSlot+i)%spareSlots);
            bool firstFrame=true, sdFramesSwept=false;
            const auto recoverSdFrames = [&] {
                if (sdFramesSwept || partialsOnFlash()) return;
                sdFramesSwept = true;
                if(recoverFrames() && persisted) save(prefs,state);
            };
            recoverSdFrames();
            for(unsigned step=0;step<orderLength;++step) {
                const unsigned slot=downloadOrder[step];
                const bool spare=slot>=firstSpareSlot;
                auto* cached=cachedFrames[slot];
                const auto* incoming=incomingFrames[slot];
                const char* path=frameCachePaths[slot];
                if(!incoming || !incoming->id[0]) {
                    LittleFS.remove(path); LittleFS.remove((String(path)+".tmp").c_str()); *cached={};
                    if(slot==2) {
                        state.solarDrawn[0]=0;
                        if(state.active==Screen::Solar) { state.active=Screen::Art; state.drawn.any=false; }
                    } else if(slot>=3 && slot<5) {
                        state.extraNames[slot-3][0]=0;
                        state.extraDrawn[slot-3][0]=0;
                        if(state.active==static_cast<Screen>(slot)) { state.active=Screen::Art; state.drawn.any=false; }
                    }
                    continue;
                }
                const bool extra=slot>=3 && slot<5;
                if(!strcmp(cached->id,incoming->id) && LittleFS.exists(path)) {
                    if(extra && strcmp(state.extraNames[slot-3],manifestExtras.names[slot-3])) {
                        strcpy(state.extraNames[slot-3],manifestExtras.names[slot-3]);
                        save(prefs,state);
                    }
                    continue;
                }
                if(frameWakeRemainingMs(millis()-wakeStarted)) {
                    // A timeout or failure still advances the spare cursor.
                    const uint8_t following=firstSpareSlot+(slot-firstSpareSlot+1)%spareSlots;
                    if(persisted && spare) prefs.putBytes("dl-next",&following,sizeof following);
                    if(extra && !canStageExtraFrame(path)) {
                        Serial.println("extra download deferred=filesystem-budget"); continue;
                    }
                    prepareDownloadStorage();
                    recoverSdFrames();
                    if(!strcmp(cached->id,incoming->id) && LittleFS.exists(path)) continue;
                    if(extra && !partialsOnFlash() && !canStageExtraFrame(path)) {
                        Serial.println("extra download deferred=filesystem-budget"); continue;
                    }
                }
                const String url=String(installation.host)+incoming->url;
                const auto remaining=frameWakeRemainingMs(millis()-wakeStarted);
                // Reserve time for flash inflate and stored-file validation before the cutoff.
                constexpr uint32_t inflateReserveMs=15000;
                const auto* expected=expectedFrame(url.c_str());
                const uint32_t reserve=expected && expected->zlib ? inflateReserveMs : 0;
                if(remaining<=reserve) {
                    if(persisted && spare) {
                        const uint8_t deferred=slot;
                        prefs.putBytes("dl-next",&deferred,sizeof deferred);
                    }
                    Serial.printf("download deferred slot=%u wake-budget\n",slot);
                    break;
                }
                const auto transferRemaining=remaining-reserve;
                const auto budget=firstFrame ? transferRemaining : std::min(transferRemaining,frameBudgetMs);
                firstFrame=false;
                if(fetchFrame(url.c_str(),path,budget)) {
                    acceptFrame(slot);
                    save(prefs,state); // Publish the ID only after the file is committed.
                }
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
        contentDue = !manifestOk;
        if (manifestOk) {
            for (unsigned slot=0; slot<frameCachePaths.size(); ++slot) {
                const auto* incoming=incomingFrames[slot];
                if (incoming && incoming->id[0] &&
                    (!mounted || strcmp(incoming->id,cachedFrames[slot]->id) != 0 ||
                     !LittleFS.exists(frameCachePaths[slot]))) {
                    contentDue = true;
                    break;
                }
            }
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
    // `shown` is the screen drawn: the scheduled one, or a cached fallback.
    State shown=state;
    if(mounted && !LittleFS.exists(selectedPath(state))) {
        for(const Screen candidate : fallbackScreens) {
            if(candidate==state.active || !screenCached(state,candidate)) continue;
            shown.active=candidate;
            break;
        }
    }
    const ScreenInfo& selected=selectedScreen(shown);
    const char* path=selectedPath(shown);
    const bool cached=mounted && LittleFS.exists(path);
    const uint32_t epochNow=ventanal::trustedEpoch(time(nullptr));
    contentStaleness.observe(epochNow,contentDue);
    if(persisted && contentStaleness.checkpointDue(epochNow)) contentStaleness.checkpoint(prefs,epochNow);
    const bool downloadFailure=contentStaleness.label(false,epochNow)==ventanal::RefreshLabel::Failed;
    const bool redraw=downloadFailure!=downloadFailureDrawn || longPress || (static_cast<unsigned>(shown.active)>=2 ? wake!=WakeCause::Timer || !shown.drawn.any || strcmp(selectedDrawn(shown),selected.id)!=0 : needsRedraw(wake,shown.active,shown.drawn,selectedArt(shown).id,shown.weather.id,pct));
    start=millis();
    if(!cached || redraw || otaPendingBoot) {
        bool ready=cached && blitFrame(path,display);
        if (cached && !ready) {
            const auto invalidate = [&](Screen screen) {
                switch (screen) {
                    case Screen::Art: if (state.owner.overrideArt.id[0]) state.owner.overrideArt = {}; else state.art.id[0] = 0; break;
                    case Screen::Weather: state.weather.id[0] = 0; break;
                    case Screen::Solar: state.solar.id[0] = 0; break;
                    case Screen::Extra0: state.extraScreens[0].id[0] = 0; break;
                    case Screen::Extra1: state.extraScreens[1].id[0] = 0; break;
                    default: break;
                }
            };
            invalidate(shown.active);
            for (const Screen candidate : fallbackScreens) {
                if (candidate == shown.active || !screenCached(state,candidate)) continue;
                shown = state; shown.active = candidate;
                if (blitFrame(selectedPath(shown),display)) { ready = true; break; }
                invalidate(candidate);
            }
        }
        Serial.printf("blit ms=%lu valid=%d\n",static_cast<unsigned long>(millis()-start),ready);
        if(ready) {
            const ScreenInfo& selected=selectedScreen(shown);
            const time_t drawnAt=time(nullptr);
            if(shown.active!=state.active)
                Serial.printf("draw fallback=%s scheduled=%s missing\n",screenName(shown),screenName(state));
            rememberPanel(shown,pct,offline,drawnAt,0,0,downloadFailure);
            const uint32_t overlayStart=millis();
            drawReadout(display,selected.readout,pct,offline,isLow(pct),drawnAt,installation.clock24h);
            Serial.printf("readout ms=%lu\n",static_cast<unsigned long>(millis()-overlayStart));
            char local[40]; tm now{}; localtime_r(&drawnAt,&now);
            strftime(local,sizeof local,"%Y-%m-%d %H:%M:%S",&now);
            Serial.printf("draw=%s updated=%s offline=%d rect=%d,%d,%d,%d\n",screenName(shown),local,offline,
                          selected.readout.x,selected.readout.y,selected.readout.w,selected.readout.h);
        } else {
            rememberPanel(state,pct,offline,time(nullptr),1,0,downloadFailure);
            drawWaiting(display,installation.host);
            Serial.println("draw=waiting-for-host");
        }
        if(downloadFailure) drawDownloadFailure(display);
        start=millis(); refreshPanel();
        downloadFailureDrawn=downloadFailure;
        if(otaPendingBoot && confirmOtaBoot(prefs,otaBoot,manifestOk,ready)) otaPendingBoot=false;
        if(!otaPendingBoot) {
            clearWatchdogCount(prefs);
            if(chargeAttempted) storeChargeMarker(prefs,0);
        }
        Serial.printf("display ms=%lu\n",static_cast<unsigned long>(millis()-start));
        state.drawn.any=ready;
        if(ready) {
            strcpy(state.drawn.artId,selectedArt(state).id);
            strcpy(state.drawn.weatherId,state.weather.id);
            strcpy(state.solarDrawn,state.solar.id);
            for(unsigned i=0;i<2;++i) strcpy(state.extraDrawn[i],state.extraScreens[i].id);
            state.drawn.pct=static_cast<int8_t>(pct);
            // The scheduled screen is not on the panel: draw it once it lands.
            if(shown.active!=state.active) drawnId(state,state.active)[0]=0;
        }
    } else {
        Serial.println("draw skipped=unchanged");
        clearWatchdogCount(prefs);
    }
#if VENTANAL_STALLTEST
    stallRun(prefs,state,pct,offline);
#endif
    if(persisted) {
        if(mounted || hadState) save(prefs,state);
        prefs.end();
    }
    if(mounted) LittleFS.end();
    const uint32_t seconds=secondsUntilNextWake(time(nullptr),installation.wakeMinutes,
                              quietStart(installation.quietStart),quietEnd(installation.quietEnd),isLow(pct));
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
