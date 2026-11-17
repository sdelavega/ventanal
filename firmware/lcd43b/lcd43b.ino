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
#include <esp_sntp.h>
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

#ifdef VENTANAL_LCD_DIAG_PANEL
// Underrun-stress builds (fix-r3-flicker.md): refresh every 30 s so a
// short capture sees several Wi-Fi download + LittleFS commit cycles
// against the live panel, and observe the VSYNC cadence (below).
constexpr uint32_t REFRESH_MS = 30 * 1000UL;
#else
constexpr uint32_t REFRESH_MS = 10 * 60 * 1000UL;
#endif
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

// Long-press feedback (host-pinned in overlay.h): REFRESHING shows on the
// gesture, UP TO DATE ~2 s when nothing changed, OFFLINE styling when the
// fetch failed; expiry redraws the cached frame clean.
lcd::RefreshLabelState refreshLabel;

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

#ifdef VENTANAL_LCD_DIAG_PANEL
// Underrun observer (ISR context): the panel's timing generator emits one
// VSYNC per frame (~39 fps at Waveshare's 16 MHz PCLK; one frame is
// 820x500 pixel clocks = 25.6 ms). A starved bounce-buffer refill makes
// the driver restart the DMA in VSYNC (the core enables
// CONFIG_LCD_RGB_RESTART_IN_VSYNC), which is the only underrun
// disturbance visible from the esp_lcd API: this core's driver exposes
// no underrun counter and logs nothing at its ERROR-only log level.
volatile uint32_t vsyncFrames = 0;
volatile uint32_t vsyncLastMs = 0;
volatile uint32_t vsyncMaxGapMs = 0;
uint32_t vsyncReportMs = 0;
uint32_t vsyncReportFrames = 0;

bool panelVsync(esp_lcd_panel_handle_t, const esp_lcd_rgb_panel_event_data_t*,
                void*) {
    const uint32_t now = millis();
    if (vsyncLastMs) {
        const uint32_t gap = now - vsyncLastMs;
        if (gap > vsyncMaxGapMs) vsyncMaxGapMs = gap;
    }
    vsyncLastMs = now;
    ++vsyncFrames;
    return false;
}

void panelVsyncReport(uint32_t now) {
    if (now - vsyncReportMs < 15000) return;
    const uint32_t frames = vsyncFrames;
    const uint32_t maxGap = vsyncMaxGapMs;
    const uint32_t window = vsyncReportMs ? (now - vsyncReportMs) / 1000 : 0;
    const uint32_t previous = vsyncReportFrames;
    vsyncReportMs = now;
    vsyncReportFrames = frames;
    vsyncMaxGapMs = 0;
    lcd::log("panel vsync frames=%lu fps=%lu max_gap_ms=%lu\n",
             static_cast<unsigned long>(frames - previous),
             window ? static_cast<unsigned long>(
                          (frames - previous) / window)
                    : 0,
             static_cast<unsigned long>(maxGap));
}
#endif

#ifdef VENTANAL_LCD_DIAG_TOUCH
// Boot diagnostics for touch bring-up (diagnostic builds only): which bus
// addresses answer, the bring-up cycle the driver accepted, the GT911
// identity registers, the full configuration block 0x8047-0x80FF plus
// the fresh flag at 0x8100, the INT line level and the CH422G output
// shadow. The GT911 datasheet expects ASCII "911" at 0x8140 and a
// published configuration (nonzero version byte, X/Y resolution,
// touch number, module switch); an all-zero block means the controller
// came up blank or wedged.
void touchBootDiag() {
    Wire.beginTransmission(Gt911::PRIMARY);
    const bool primary = Wire.endTransmission() == 0;
    Wire.beginTransmission(Gt911::SECONDARY);
    const bool secondary = Wire.endTransmission() == 0;
    lcd::log("diag probe 0x%02x=%d 0x%02x=%d addr=0x%02x cycles=%d int=%d"
             " ch422g_io=0x%02x\n",
             Gt911::PRIMARY, primary, Gt911::SECONDARY, secondary,
             touch.address(), int(touch.cycles()), digitalRead(4),
             expander.outputs());
    uint8_t pid[3] = {}, fw[2] = {};
    const bool pidOk = touch.peek(0x8140, pid, sizeof pid);
    const bool fwOk = touch.peek(0x8144, fw, sizeof fw);
    lcd::log("diag gt911 product=%c%c%c ok=%d fw=0x%02x%02x ok=%d\n",
             pid[0], pid[1], pid[2], pidOk, fw[0], fw[1], fwOk);
    // Dump the configuration block in 16-byte reads: a single 185-byte
    // requestFrom exceeds the core's Wire receive buffer and fails.
    uint8_t cfg[185] = {};   // 0x8047..0x80FF, checksum last
    bool cfgOk = true;
    for (size_t off = 0; off < sizeof cfg; off += 16) {
        const size_t len = off + 16 <= sizeof cfg ? size_t(16)
                                                  : sizeof cfg - off;
        if (!touch.peek(uint16_t(0x8047 + off), cfg + off, len)) cfgOk = false;
        char line[88];
        int used = snprintf(line, sizeof line, "diag cfg %04x:",
                            0x8047 + int(off));
        if (used < 0) continue;
        for (size_t i = 0; i < len; ++i) {
            const int written =
                used < int(sizeof line)
                    ? snprintf(line + used, sizeof line - size_t(used),
                               " %02x", cfg[off + i])
                    : 0;
            if (written <= 0) break;
            used += written;
        }
        lcd::log("%s\n", line);
    }
    uint8_t fresh = 0;
    const bool freshOk = touch.peek(0x8100, &fresh, 1);
    // Config multi-byte fields are little-endian (unlike the big-endian
    // point coordinates): 800 -> 20 03, 480 -> e0 01.
    lcd::log("diag cfg ok=%d version=%d res=%dx%d touch_num=%d"
             " module_switch=0x%02x checksum=0x%02x fresh=%d fresh_ok=%d\n",
             cfgOk, cfg[0], int(cfg[2]) << 8 | cfg[1],
             int(cfg[4]) << 8 | cfg[3], cfg[5], cfg[6], cfg[184], fresh,
             freshOk);
    lcd::log("OWNER: tap now\n");
}
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
    if (const char* text = lcd::refreshLabelText(refreshLabel.label())) {
        lcd::drawRefreshLabel(pixels, lcd::WIDTH, lcd::HEIGHT, text,
                              refreshLabel.label() == lcd::RefreshLabel::Offline
                                  ? lcd::RGB565_RED
                                  : lcd::RGB565_WHITE);
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

// Log every label transition so captures prove the feedback cycle.
void logRefreshLabel() {
    const char* text = lcd::refreshLabelText(refreshLabel.label());
    lcd::log("label=%s\n", text ? text : "none");
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
    // Did anything the screen shows change? frameOk slots that merely
    // re-stage the current id (skipped=unchanged) are not new content.
    bool changed = false;
    if (refreshStaging.manifestOk) {
        for (int slot = 0; slot < lcd::SCREEN_SLOTS; ++slot) {
            if (refreshStaging.dropSlot[slot]
                || (refreshStaging.frameOk[slot]
                    && strcmp(refreshStaging.frameIds[slot],
                              state.cachedIds[slot]))) {
                changed = true;
                break;
            }
        }
    }
    const bool redraw = lcd::applyRefresh(state, screenIndex, spareCursor,
                                          prefs, refreshStaging,
                                          refreshRequested);
    xSemaphoreGive(refreshCopied);
    refreshBusy = false;
    if (refreshRequested)
        refreshLabel.finish(millis(), !refreshStaging.manifestOk, changed);
    else if (redraw)
        refreshLabel.clear();   // a scheduled redraw replaces the held label
    logRefreshLabel();
    lcd::log("refresh fetch=%lu ok=%d offline=%d\n", fetchCounter++,
             refreshStaging.manifestOk, state.offline);
    if (screenIndex != previousIndex)   // normalised to a shrunk list
        lcd::log("screen=%s\n", lcd::SLOT_NAMES[screenSlot(screenIndex)]);
    if (redraw) show(screenIndex);
    else if (refreshLabel.label() != lcd::RefreshLabel::None)
        show(screenIndex);   // redraw the cached frame with the label
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
        // Non-blocking: the worker refreshes while touch keeps working,
        // and the label shows immediately that the gesture registered.
        refreshLabel.begin(millis());
        logRefreshLabel();
        if (!startRefresh(true)) {
            if (refreshBusy)
                refreshRequested = true;   // adopt the in-flight pass
            else
                refreshLabel.clear();       // nothing will finish it
        }
        show(screenIndex);             // redraw the frame with the label
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
    // The CH422G shadow proves no later expander write re-asserted
    // TP_RST (bit 1) while touch was live.
    if (sample.report != lastRawSample.report
        || now - lastRawLogMs >= 1000) {
        lcd::log("touch raw=%d status=0x%02x contact=%d x=%d y=%d int=%d"
                 " ch422g_io=0x%02x\n",
                 int(sample.report), sample.status, sample.contact, x, y,
                 digitalRead(4), expander.outputs());
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
    // Initialise the configured timezone BEFORE any refresh (and independent
    // of connectivity): an offline reboot with retained RTC time evaluates
    // the persisted LOCAL quiet window in the configured zone, not UTC.
    lcd::configureTimezone();
    if (!expander.begin()) lcd::log("error=ch422g-begin\n");
    expander.setBacklight(false);
    if (!panel.begin(setLcdResetLine)) lcd::log("error=panel-begin\n");
    if (!touch.begin(4, setTouchResetLine)) {
        lcd::log("error=gt911-begin\n");
        scanBus();
    } else {
        lcd::log("touch gt911 address=0x%02x cycles=%d\n", touch.address(),
                 int(touch.cycles()));
    }
#ifdef VENTANAL_LCD_DIAG_TOUCH
    touchBootDiag();
#endif
#ifdef VENTANAL_LCD_DIAG_PANEL
    {   // Stress build: observe the VSYNC cadence and start with a cold
        // frame cache so the first refresh re-downloads and re-commits
        // all slots (real Wi-Fi payload plus LittleFS erase/program
        // traffic) while the panel is streaming. The cache self-heals on
        // the next accepted manifest.
        esp_lcd_rgb_panel_event_callbacks_t callbacks = {};
        callbacks.on_vsync = panelVsync;
        if (esp_lcd_rgb_panel_register_event_callbacks(panel.handle(),
                                                       &callbacks, nullptr)
            != ESP_OK)
            lcd::log("diag panel error=vsync-callback\n");
        for (auto& id : state.cachedIds) id[0] = 0;
        lcd::log("diag panel stress=cold-cache vsync=on\n");
    }
#endif
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
    // The label hold (~2 s) expires here: redraw the cached frame clean.
    if (refreshLabel.tick(now)) {
        logRefreshLabel();
        show(screenIndex);
    }
#ifdef VENTANAL_LCD_DIAG_PANEL
    panelVsyncReport(now);
#endif
    if (refreshBusy && xSemaphoreTake(refreshStaged, 0) == pdTRUE)
        applyFinishedRefresh();
    if (!refreshBusy && now - lastFetchMs >= REFRESH_MS) startRefresh(false);
    delay(2);
}
