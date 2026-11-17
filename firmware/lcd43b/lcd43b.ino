// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
//
// Ventanal phase-1 firmware for the Waveshare ESP32-S3-Touch-LCD-4.3B.
// The board is USB powered (no battery, no deep sleep): it fetches the
// per-device manifest every ten minutes and on demand, hash-verifies and
// caches changed VNR1 RGB565 frames in LittleFS, and blits the selected
// screen to the 800x480 panel. Touch: tap right half = next screen, left
// half = previous screen, double tap = next spare artwork, press >= 1.5 s
// = refresh now. The backlight follows the manifest's (persisted) quiet
// hours; any touch lights it for 60 s during quiet hours. Offline keeps
// the cached screen with a small OFFLINE mark.
//
// Refreshes run on a short-lived FreeRTOS worker task supervised by the
// task watchdog, so loop() keeps polling touch and servicing the
// backlight while Wi-Fi waits, NTP waits and up to six 60 s transfers
// run. The worker only performs network I/O and frame staging; every
// state change is applied by loop() through lcd::applyRefresh, so UI
// state has a single writer (host-tested in sync.h).

#include <Arduino.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <Wire.h>
#include <esp_task_wdt.h>
#include <cstdio>
#include <ctime>

#ifdef VENTANAL_LCD_CONFIG
#include VENTANAL_LCD_CONFIG
#else
#include "config.example.h"
#endif
// VENTANAL_LCD_CONFIG is the only private-configuration path: the sketch
// never falls back to a local config.h, so private values cannot sneak
// into the repository.

#include "ch422g.h"
#include "frame.h"
#include "gesture.h"
#include "gt911.h"
#include "log.h"
#include "manifest.h"
#include "overlay.h"
#include "panel.h"
#include "persist.h"
#include "screens.h"
#include "sync.h"
#include "touch.h"
#ifdef VENTANAL_LCD_DIAG_GESTURES
#include "diag.h"
#endif

#if defined(VENTANAL_LCD_DIAG_TOUCH) && defined(VENTANAL_LCD_DIAG_GESTURES)
#error VENTANAL_LCD_DIAG_TOUCH and VENTANAL_LCD_DIAG_GESTURES drive the touch pipeline from different sources; build with at most one.
#endif

// Optional private-config keys with safe defaults.
#ifndef VENTANAL_DEVICE_ID
#define VENTANAL_DEVICE_ID "lcd43b"
#endif
#ifndef VENTANAL_TZ
#define VENTANAL_TZ "MST7MDT,M3.2.0,M11.1.0"
#endif

namespace {

constexpr uint32_t REFRESH_MS = 10 * 60 * 1000UL;
constexpr uint32_t TOUCH_WAKE_MS = 60000;
constexpr uint32_t TOUCH_POLL_MS = 10;
constexpr uint32_t WORKER_STACK_BYTES = 16384;

Ch422g expander{Wire};
Gt911 touch{Wire};
Panel panel;
lcd::TouchPoller touchPoller{touch};
Preferences prefs;

lcd::RefreshState state;   // manifest, cache ids, offline flag
int screenIndex = 0;       // current screen within the screen list
int spareCursor = 0;       // next spare shown by a double tap

bool backlightState = false;
uint32_t touchUntilMs = 0;
uint32_t lastFetchMs = 0;
uint32_t lastTouchPollMs = 0;
bool lastTouched = false;
uint32_t fetchCounter = 0;

#ifdef VENTANAL_LCD_DIAG_GESTURES
// One-shot diagnostic gesture replay (see diag.h), armed in setup() and
// disabled once the synthetic sequence completes.
bool diagActive = false;
uint32_t diagStartMs = 0;
#endif
#ifdef VENTANAL_LCD_DIAG_TOUCH
// Rate-limited raw-report logging for touch captures: every report change
// plus at most one line per second while a finger is held.
uint32_t lastRawLogMs = 0;
lcd::TouchSample lastRawSample{};
#endif

// Diagnostic only: report every I2C address that acknowledges a probe.
void scanBus() {
    char line[160] = "i2c scan:";
    size_t used = strlen(line);
    for (uint8_t address = 1; address < 0x78; ++address) {
        Wire.beginTransmission(address);
        if (Wire.endTransmission() == 0 && used < sizeof line - 8)
            used += snprintf(line + used, sizeof line - used, " %02x", address);
    }
    lcd::log("%s\n", line);
}

// Screen list helpers over the live (persisted or freshly fetched) shape.
bool solarPublished() { return state.manifest.screens[2].present; }
int spareCount() { return state.manifest.spareCount; }

bool setTouchResetLine(bool asserted) { return expander.setTouchReset(asserted); }
bool setLcdResetLine(bool asserted) { return expander.setLcdReset(asserted); }

int screenCount() { return lcd::screenCount(solarPublished(), spareCount()); }
int spareBase() { return lcd::spareBase(solarPublished()); }

int screenSlot(int index) {
    return lcd::screenSlot(index, solarPublished(), spareCount());
}

void storeScreenIndex() { lcd::saveSelection(prefs, screenIndex, spareCursor); }

// Blit a cached frame (plus overlays) to the panel through a PSRAM buffer.
bool show(int index) {
    const int slot = screenSlot(index);
    if (slot < 0) {   // persisted index outside the current screen list
        lcd::log("blit error=screen-index index=%d\n", index);
        return false;
    }
    const String path = String("/") + lcd::SLOT_NAMES[slot] + ".vnr";
    uint16_t* pixels = static_cast<uint16_t*>(
        ps_malloc(size_t(lcd::WIDTH) * lcd::HEIGHT * 2));
    if (!pixels) {
        lcd::log("blit error=allocation\n");
        return false;
    }
    const uint32_t started = millis();
    bool ok = false;
    if (state.cachedIds[slot][0] && LittleFS.exists(path)) {
        File file = LittleFS.open(path, "r");
        if (file) {
            uint8_t header[lcd::HEADER_BYTES] = {};
            if (file.read(header, lcd::HEADER_BYTES) == int(lcd::HEADER_BYTES)
                && lcd::headerValid(header)
                && file.read(reinterpret_cast<uint8_t*>(pixels),
                             size_t(lcd::WIDTH) * lcd::HEIGHT * 2)
                        == int(size_t(lcd::WIDTH) * lcd::HEIGHT * 2)) {
                ok = true;
            }
            file.close();
        }
    }
    if (!ok) {
        lcd::drawWaiting(pixels, lcd::WIDTH, lcd::HEIGHT, VENTANAL_HOST_URL);
    } else if (state.offline) {
        lcd::drawOfflineMark(pixels, lcd::WIDTH, lcd::HEIGHT);
    }
    panel.blit(pixels);
    free(pixels);
    state.drawn = ok;
    lcd::log("blit screen=%s id=%.16s cache=%s offline=%d ms=%lu\n",
             lcd::SLOT_NAMES[slot], state.cachedIds[slot],
             ok ? "frame" : "waiting", state.offline,
             static_cast<unsigned long>(millis() - started));
    return ok;
}

void setBacklight(bool on) {
    if (on == backlightState) return;
    backlightState = on;
    expander.setBacklight(on);
    lcd::log("backlight on=%d\n", on);
}

// The quiet window comes from the live manifest (persisted at boot, then
// refreshed with every accepted manifest) and only applies once the wall
// clock is trustworthy: before the first NTP sync the backlight stays on.
void backlightRefresh() {
    tm local{};
    const time_t now = time(nullptr);
    localtime_r(&now, &local);
    const bool quiet = lcd::quietApplies(local.tm_year + 1900,
                                         local.tm_hour * 60 + local.tm_min,
                                         state.manifest);
    setBacklight(!quiet || millis() < touchUntilMs);
}

// --- Refresh worker ------------------------------------------------------

// Task-watchdog subscription for the worker: armed once at boot, the
// worker feeds it on every progress step (sync.cpp), so a wedged network
// call beyond the window resets the board instead of freezing refreshes.
class TaskWatchdog : public lcd::Watchdog {
public:
    void arm() {
        // The Arduino core starts its own TWDT, so reconfigure it here
        // (quietly); a system without one initialises it fresh instead.
        const esp_task_wdt_config_t config{lcd::WATCHDOG_TIMEOUT_MS, 0, true};
        const esp_err_t result = esp_task_wdt_reconfigure(&config);
        if (result == ESP_ERR_INVALID_STATE)
            ESP_ERROR_CHECK(esp_task_wdt_init(&config));
        else
            ESP_ERROR_CHECK(result);
    }
    void subscribe() {
        ESP_ERROR_CHECK(
            esp_task_wdt_add_user("ventanal-refresh", &handle_));
    }
    void feed() override { esp_task_wdt_reset_user(handle_); }
    void unsubscribe() { ESP_ERROR_CHECK(esp_task_wdt_delete_user(handle_)); }

private:
    esp_task_wdt_user_handle_t handle_ = nullptr;
};

TaskWatchdog refreshWatchdog;
SemaphoreHandle_t refreshStaged;   // worker -> loop: result ready to copy
SemaphoreHandle_t refreshCopied;   // loop -> worker: result applied
volatile bool refreshBusy = false;
bool refreshRequested = false;
lcd::RefreshResult refreshStaging;   // worker writes, loop copies while parked

void refreshWorker(void*) {
    refreshWatchdog.subscribe();
    lcd::runRefresh(VENTANAL_HOST_URL, VENTANAL_DEVICE_ID, state.cachedIds,
                    refreshStaging, refreshWatchdog);
    refreshWatchdog.unsubscribe();
    xSemaphoreGive(refreshStaged);
    // Park until loop() has copied the result, so the staging buffer is
    // never read while the worker might still touch it.
    xSemaphoreTake(refreshCopied, portMAX_DELAY);
    vTaskDelete(nullptr);
}

// Kick a refresh on the worker task; loop() keeps servicing touch and the
// backlight while it runs. At most one worker may be in flight.
bool startRefresh(bool redraw) {
    if (refreshBusy) return false;
    refreshRequested = redraw;
    lastFetchMs = millis();
    refreshBusy = true;
    if (xTaskCreate(refreshWorker, "ventanal-refresh", WORKER_STACK_BYTES,
                    nullptr, 1, nullptr) != pdPASS) {
        refreshBusy = false;
        lcd::log("refresh error=task-create\n");
        return false;
    }
    return true;
}

// Apply a finished worker pass (single writer: loop()).
void applyFinishedRefresh() {
    const int previousIndex = screenIndex;
    const bool redraw = lcd::applyRefresh(state, screenIndex, spareCursor,
                                          prefs, refreshStaging,
                                          refreshRequested);
    xSemaphoreGive(refreshCopied);
    refreshBusy = false;
    lcd::log("refresh fetch=%lu ok=%d offline=%d\n", fetchCounter++,
             refreshStaging.manifestOk, state.offline);
    if (screenIndex != previousIndex)   // normalised to a shrunk list
        lcd::log("screen=%s\n", lcd::SLOT_NAMES[screenSlot(screenIndex)]);
    if (redraw) show(screenIndex);
    backlightRefresh();
}

// --- Touch ----------------------------------------------------------------

void handleGesture(Gesture gesture, int x, int y) {
    const char* name = gesture == Gesture::Next      ? "next"
                     : gesture == Gesture::Previous  ? "previous"
                     : gesture == Gesture::DoubleTap ? "double"
                                                      : "refresh";
    lcd::log("gesture=%s x=%d y=%d\n", name, x, y);
    const int count = screenCount();
    switch (gesture) {
    case Gesture::Next:
        screenIndex = (screenIndex + 1) % count;
        lcd::log("screen=%s\n", lcd::SLOT_NAMES[screenSlot(screenIndex)]);
        show(screenIndex);
        break;
    case Gesture::Previous:
        screenIndex = (screenIndex + count - 1) % count;
        lcd::log("screen=%s\n", lcd::SLOT_NAMES[screenSlot(screenIndex)]);
        show(screenIndex);
        break;
    case Gesture::DoubleTap:
        if (spareCount()) {
            screenIndex = spareBase() + spareCursor;
            spareCursor = (spareCursor + 1) % spareCount();
            lcd::log("screen=%s\n", lcd::SLOT_NAMES[screenSlot(screenIndex)]);
            show(screenIndex);
        }
        break;
    case Gesture::Refresh:
        // Non-blocking: the worker refreshes while touch keeps working.
        if (!startRefresh(true)) lcd::log("refresh busy=1\n");
        break;
    default:
        break;
    }
    storeScreenIndex();
    backlightRefresh();
}

void pollTouch(uint32_t now) {
    if (now - lastTouchPollMs < TOUCH_POLL_MS) return;
    lastTouchPollMs = now;
    Gesture gesture = Gesture::None;
    bool touched = false;
    int x = 0, y = 0;
#ifdef VENTANAL_LCD_DIAG_GESTURES
    if (diagActive && diagStartMs && now >= diagStartMs) {
        // Synthetic samples through the real classifier (see diag.h);
        // real touch takes over again once the sequence completes.
        bool synthetic = false;
        if (!lcd::diagTouchSample(now - diagStartMs, synthetic, x, y)) {
            diagActive = false;
            touched = false;
            lcd::log("diag gestures=done\n");
        } else {
            gesture = touchPoller.sampleSynthetic(now, synthetic, x, y);
            touched = synthetic;
        }
    } else {
        lcd::TouchSample sample;
        gesture = touchPoller.poll(now, &sample);
        touched = sample.contact;
        x = sample.x;
        y = sample.y;
    }
#else
    lcd::TouchSample sample;
    gesture = touchPoller.poll(now, &sample);
    touched = sample.contact;
    x = sample.x;
    y = sample.y;
#endif
#ifdef VENTANAL_LCD_DIAG_TOUCH
    // Every report change, plus at most one line per second while held.
    if (sample.report != lastRawSample.report
        || now - lastRawLogMs >= 1000) {
        lcd::log("touch raw=%d status=0x%02x contact=%d x=%d y=%d\n",
                 int(sample.report), sample.status, sample.contact, x, y);
        lastRawLogMs = now;
    }
    lastRawSample = sample;
#endif
    if (touched && !lastTouched) touchUntilMs = now + TOUCH_WAKE_MS;
    lastTouched = touched;
    if (gesture != Gesture::None) handleGesture(gesture, x, y);
}

}  // namespace

void setup() {
#if ARDUINO_USB_CDC_ON_BOOT && ARDUINO_USB_MODE
    // HWCDC (native USB-Serial/JTAG): keep the last boot lines while no
    // host has the port open (the not-connected write path drops only the
    // oldest queued bytes), and bound every write even if a host stalls.
    Serial.setTxBufferSize(4096);
    Serial.setTxTimeoutMs(20);
#endif
    Serial.begin(115200);
    delay(300);
    lcd::log("ventanal lcd43b boot\n");
    lcd::log("usb cdc hw=%d onboot=%d\n", ARDUINO_USB_MODE,
             ARDUINO_USB_CDC_ON_BOOT);
    Wire.begin(8, 9);   // SDA=GPIO8, SCL=GPIO9, shared by expander and touch
    Wire.setClock(400000);
    if (!prefs.begin("ventanal", false)) lcd::log("error=nvs-open\n");
    // Mount the partition named "littlefs" (spiffs subtype); first boot formats.
    if (!LittleFS.begin(true, "/littlefs", 10, "littlefs"))
        lcd::log("error=littlefs-mount\n");
    // Restore persisted state so an offline boot still honours quiet hours
    // and the screen list matches the cache before the first fetch.
    lcd::loadCacheIds(prefs, state.cachedIds);
    lcd::loadSelection(prefs, screenIndex, spareCursor);
    lcd::loadQuiet(prefs, state.manifest);
    lcd::loadPublishShape(prefs, state.manifest);
    lcd::normalizeSelection(screenIndex, spareCursor, solarPublished(),
                            spareCount());
    if (!expander.begin()) lcd::log("error=ch422g-begin\n");
    expander.setBacklight(false);
    if (!panel.begin(setLcdResetLine)) lcd::log("error=panel-begin\n");
    if (!touch.begin(4, setTouchResetLine)) {
        lcd::log("error=gt911-begin\n");
        scanBus();
    } else {
        lcd::log("touch gt911 address=0x%02x\n", touch.address());
    }
    refreshWatchdog.arm();
    refreshStaged = xSemaphoreCreateBinary();
    refreshCopied = xSemaphoreCreateBinary();
    show(screenIndex);            // cached frame, or the waiting screen
    startRefresh(true);
#ifdef VENTANAL_LCD_DIAG_GESTURES
    // Run the synthetic gesture sequence (host-tested in diag.h) a couple
    // of seconds after the first refresh, so a capture taken without
    // touching the panel still exercises the whole gesture pipeline.
    diagActive = true;
    diagStartMs = millis() + 2000;
    lcd::log("diag gestures=planned\n");
#endif
}

void loop() {
    const uint32_t now = millis();
    pollTouch(now);
    backlightRefresh();
    if (refreshBusy && xSemaphoreTake(refreshStaged, 0) == pdTRUE)
        applyFinishedRefresh();
    if (!refreshBusy && now - lastFetchMs >= REFRESH_MS) startRefresh(false);
    delay(2);
}
