// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
//
// Ventanal phase-1 firmware for the Waveshare ESP32-S3-Touch-LCD-4.3B.
// The board applies its configured idle timeout on USB and battery. It fetches the
// per-device manifest every ten minutes and on demand, hash-verifies and
// displays changed VNR1 RGB565 frames from PSRAM on the 800x480 panel.
// It saves their original zlib payloads when the screen is dark or the
// save deadline expires. Touch: tap right half = next screen, left
// half = previous screen, double tap = next spare artwork, press >= 1.5 s
// = refresh now. The backlight follows the manifest's (persisted) quiet
// hours; any touch lights it for 60 s during quiet hours. Offline keeps
// the cached screen with a small OFFLINE mark.
//
// Refreshes run on a short-lived FreeRTOS worker task supervised by the
// task watchdog, so loop() keeps polling touch and servicing the
// backlight while Wi-Fi waits, NTP waits and up to six 60 s transfers
// run. The worker only performs network I/O and frame staging; every
// state change and cache write is applied by loop(), so UI and flash
// cache state have a single writer (host-tested in sync.h).

#include <Arduino.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <Wire.h>
#include <WiFi.h>
#include <esp_task_wdt.h>
#include <esp_sleep.h>
#include <esp_heap_caps.h>
#if ARDUINO_USB_CDC_ON_BOOT && ARDUINO_USB_MODE
#include <hal/usb_serial_jtag_ll.h>
#endif
#include <driver/gpio.h>
#include <cstdio>
#include <ctime>
#include <sys/time.h>
#include <atomic>
#include "clock.h"

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
#include "inflate.h"
#include "gesture.h"
#include "gt911.h"
#include "log.h"
#include "manifest.h"
#include "overlay.h"
#include "panel.h"
#include "power_session.h"
#include "persist.h"
#include "owner.h"
#include "screens.h"
#include "settings_page.h"
#include "sync.h"
#include "touch.h"
#include <esp_sntp.h>
#ifdef VENTANAL_LCD_DIAG_GESTURES
#include "diag.h"
#endif

#ifdef VENTANAL_LCD_DIAG_HW
#include "hwprobe.h"
#endif

#if (defined(VENTANAL_LCD_DIAG_TOUCH) + defined(VENTANAL_LCD_DIAG_GESTURES) + defined(VENTANAL_LCD_DIAG_HW)) > 1
#error Build with at most one of VENTANAL_LCD_DIAG_TOUCH, VENTANAL_LCD_DIAG_GESTURES, VENTANAL_LCD_DIAG_HW.
#endif
#if defined(VENTANAL_LCD_DIAG_HW) && defined(VENTANAL_LCD_DIAG_PANEL)
#error Hardware probing and panel stress require separate builds.
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
Preferences settingsPrefs; // exclusively opened/closed by settings persistence
lcd::DeviceSettings settings=lcd::defaultSettings(lcd::POWER_SOURCE_DETECTABLE);
lcd::SettingsSession settingsPage;
int settingsReturnSlot=0;
bool settingsRefreshPending=false;
// This board exposes no battery ADC. Keep calibration gated by both hardware
// capability and a valid measurement; never synthesize a full-cell reading.
int settingsBatteryMv() { return -1; }
bool settingsBatteryButton() {
    if constexpr(!lcd::BATTERY_ADC_AVAILABLE) return false;
    return settingsBatteryMv()>0;
}

lcd::RefreshState state;   // manifest, cache ids, offline flag
uint8_t* residentFrames[lcd::SCREEN_SLOTS]{};
uint8_t* overrideFrame = nullptr;
lcd::ResidentCache refreshCache;
bool mounted = false;
lcd::CacheLocation persistedFrames[lcd::SCREEN_SLOTS];
lcd::DeferredSave deferredFrames[lcd::SCREEN_SLOTS];
lcd::DeferredSave deferredOverride;
int overrideSource = -1;  // persisted spare to copy if no wire payload is in RAM
int screenIndex = 0;       // current screen within the screen list
int spareCursor = -1;      // legacy selection key; OwnerState owns swapping
OwnerState owner;
uint32_t skipRevision=0, sentSkipRevision=0;
char refreshSkips[280]{};

// Long-press feedback (host-pinned in overlay.h): REFRESHING shows on the
// gesture, UP TO DATE ~2 s when nothing changed, OFFLINE styling when the
// fetch failed; expiry redraws the cached frame clean.
lcd::RefreshLabelState refreshLabel;

lcd::PowerSession powerSession;
// There is no voltage or VBUS signal on this board. No invented reading.
lcd::BatteryLevel powerLevel() {
    const int mv=settingsBatteryMv();
    return lcd::batteryLevel(mv,lcd::batteryPercent(mv,settings.batteryFullMv));
}
uint32_t powerPollMs() {
#ifdef VENTANAL_LCD_DIAG_PANEL
    return REFRESH_MS;
#else
    return lcd::pollIntervalMs(powerSession.screenOn,powerLevel());
#endif
}
bool backlightState = false;
bool quietBlanking = false;
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
uint32_t stressStartMs = 0;
unsigned stressCycles = 0;

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

int screenCount() {
    int slots[lcd::SCREEN_SLOTS];
    return lcd::activeSlots(solarPublished(),spareCount(),settings,slots,lcd::SCREEN_SLOTS);
}

int screenSlot(int index) {
    return lcd::screenSlot(index, solarPublished(), spareCount(),settings);
}

void restartScanout(const char* operation) {
    if (!panel.handle()) return;
    const esp_err_t status = esp_lcd_rgb_panel_restart(panel.handle());
#ifdef VENTANAL_LCD_DIAG_PANEL
    lcd::log("panel restart after=%s status=%d\n", operation, int(status));
#else
    if (status != ESP_OK)
        lcd::log("panel restart after=%s status=%d\n", operation, int(status));
#endif
}

void storeScreenIndex() {
    lcd::saveSelection(prefs, screenIndex, spareCursor);
    restartScanout("selection");
}

// Only loop() owns these packed buffers. The worker never reads or writes
// them; minute ticks perform no filesystem I/O and no full-frame blits.
uint16_t* clockBackground = nullptr;
uint16_t* clockPixels = nullptr;
uint16_t* clockDimmed = nullptr;
int clockSlot = -1;

String residentPath(int slot, bool alternate) {
    return String("/") + lcd::SLOT_NAMES[slot] + (alternate ? "-alt.vnr" : ".vnr");
}

bool loadResidentFrame(const char* path, uint8_t* frame, const char* id) {
    if (!mounted || !frame || strlen(id) != 16) return false;
    File file = LittleFS.open(path, "r");
    const size_t bytes = file ? file.size() : 0;
    if (bytes >= lcd::HEADER_BYTES && bytes <= lcd::FRAME_WIRE_MAX) {
        // An old cache stores a raw VNR1 frame. The zlib wrapper starts
        // with a different magic, even if it happens to have the same size.
        if (bytes == lcd::FRAME_BYTES && file.read(frame, lcd::HEADER_BYTES) == lcd::HEADER_BYTES
            && lcd::headerValid(frame)) {
            if (file.read(frame + lcd::HEADER_BYTES, lcd::PIXEL_BYTES) == lcd::PIXEL_BYTES
                && lcd::cachedFrameValid(frame, id)) return true;
        } else if (file.seek(0)) {
            auto* wire = static_cast<uint8_t*>(ps_malloc(bytes));
            if (wire) {
                const bool valid = file.read(wire, bytes) == bytes
                    && lcd::frameHashMatches(wire, bytes, id, 16)
                    && lcd::decodeFrame(lcd::FrameEncoding::Zlib, wire, bytes, frame);
                free(wire);
                if (valid) return true;
            }
        }
    }
    lcd::log("cache boot path=%s ok=0\n", path);
    return false;
}

void loadResidentCache() {
    const uint32_t started = millis();
    constexpr size_t clockBytes = 250 * 80 * sizeof(uint16_t);
    clockBackground = static_cast<uint16_t*>(ps_malloc(clockBytes));
    clockPixels = static_cast<uint16_t*>(ps_malloc(clockBytes));
    clockDimmed = static_cast<uint16_t*>(ps_malloc(clockBytes));
    if (!clockBackground || !clockPixels || !clockDimmed)
        lcd::log("clock error=allocation\n");
    unsigned allocated = 0;
    unsigned loaded = 0;
    constexpr int firstSpare = lcd::SCREEN_SLOTS - lcd::SPARE_SLOTS;
    for (int slot = 0; slot < lcd::SCREEN_SLOTS; ++slot) {
        residentFrames[slot] = static_cast<uint8_t*>(ps_malloc(lcd::FRAME_BYTES));
        refreshCache.allocated[slot] = residentFrames[slot] != nullptr;
        if (residentFrames[slot]) ++allocated;
        const auto& saved = persistedFrames[slot];
        const String path = residentPath(slot, saved.alternate);
        if (loadResidentFrame(path.c_str(), residentFrames[slot], saved.id)) {
            strcpy(state.cachedIds[slot], saved.id);
            ++loaded;
        }
    }
    const bool mainChanged = state.cachedIds[0][0] && strcmp(owner.mainId, state.cachedIds[0]);
    if (mainChanged) {
        owner.mainChanged(state.cachedIds[0]);
        lcd::saveOwner(prefs, owner);
    }
    if (owner.overrideArt.id[0]) {
        overrideFrame = static_cast<uint8_t*>(ps_malloc(lcd::FRAME_BYTES));
        bool restored = loadResidentFrame("/override.vnr", overrideFrame, owner.overrideArt.id);
        for (int slot = firstSpare; !restored && overrideFrame && slot < lcd::SCREEN_SLOTS; ++slot) {
            if (strcmp(state.cachedIds[slot], owner.overrideArt.id) != 0) continue;
            memcpy(overrideFrame, residentFrames[slot], lcd::FRAME_BYTES);
            deferredOverride.replace(owner.overrideArt.id, {}, millis());
            overrideSource = slot;
            restored = true;
        }
        if (!restored) {
            free(overrideFrame);
            overrideFrame = nullptr;
            owner.overrideArt = {};
            owner.cursor = -1;
            lcd::saveOwner(prefs, owner);
        }
    }
    refreshCache.staging = static_cast<uint8_t*>(ps_malloc(lcd::FRAME_BYTES));
    const size_t clockUsed = clockBytes * (unsigned(clockBackground != nullptr) +
        unsigned(clockPixels != nullptr) + unsigned(clockDimmed != nullptr));
    const size_t panelBytes = panel.handle() ? lcd::PIXEL_BYTES : 0;
    const size_t rxBytes = refreshCache.staging ? lcd::FRAME_BYTES : 0;
    const size_t slotBytes = allocated * lcd::FRAME_BYTES;
    const size_t overrideBytes = overrideFrame ? lcd::FRAME_BYTES : 0;
    lcd::log("psram total=%u free=%u largest=%u framebuf=%u rx=%u"
             " clock=%u slots=%u/%u loaded=%u cache_bytes=%u override=%u"
             " display_total=%u load_ms=%lu\n",
             unsigned(ESP.getPsramSize()), unsigned(ESP.getFreePsram()),
             unsigned(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM)),
             unsigned(panelBytes), unsigned(rxBytes),
             unsigned(clockUsed), allocated, unsigned(lcd::SCREEN_SLOTS), loaded,
             unsigned(slotBytes), unsigned(overrideBytes),
             unsigned(panelBytes + rxBytes + clockUsed + slotBytes + overrideBytes),
             static_cast<unsigned long>(millis() - started));
    restartScanout("boot-cache");
}
time_t drawnMinute = -1;
std::atomic<bool> ntpRtcPending{false};
void onNtpSync(timeval*) { ntpRtcPending.store(true); }

bool renderClock() {
    tm local{}; const time_t now=time(nullptr); localtime_r(&now,&local);
    char timeText[16], dateText[24];
    const bool valid=lcd::formatClock(local,state.manifest.clock24h,timeText,sizeof timeText,dateText,sizeof dateText);
    const int mv=settingsBatteryMv();
    const int percent=lcd::batteryPercent(mv,settings.batteryFullMv);
    lcd::renderReadout(clockPixels,clockBackground,settingsPage.active || lcd::isLcarsSlot(clockSlot),
                       timeText,dateText,percent,powerLevel());
    lcd::log("readout valid=%d time=%s date=%s h24=%d battery=none\n",
             valid,timeText,dateText,state.manifest.clock24h);
    drawnMinute=now/60;
    return valid;
}
void tickClock() {
    if(!powerSession.screenOn || powerSession.chargeShowing || clockSlot<0 || (!settingsPage.active && !state.drawn) || time(nullptr)/60==drawnMinute) return;
    const bool valid=renderClock(); const auto r=lcd::readoutRect(clockSlot);
    const bool ok=panel.blitRegionDimmed(r.x,r.y,r.w,r.h,clockPixels,clockDimmed,settings.brightness);
    lcd::log("clock minute valid=%d slot=%s region=%d,%d,%d,%d blit=%d brightness=%u battery=none\n",
             valid,settingsPage.active?"settings":lcd::SLOT_NAMES[clockSlot],r.x,r.y,r.w,r.h,ok,settings.brightness);
}
void seedRtc() {
    if constexpr(lcd::RTC_PCF85063) {
        tm utc{};
        if(lcd::readRtc(Wire,utc)) {
            timeval value{lcd::rtcEpoch(utc),0}; settimeofday(&value,nullptr);
            lcd::log("rtc seed=valid utc=1\n");
        } else lcd::log("rtc seed=skipped invalid-or-OS\n");
    }
}
void saveRtcAfterNtp() {
    if constexpr(lcd::RTC_PCF85063) {
        const time_t now=time(nullptr); tm utc{}; gmtime_r(&now,&utc);
        const bool written=lcd::writeRtc(Wire,utc);
        tm check{}; const bool verified=written && lcd::readRtc(Wire,check)
                            && std::abs(double(lcd::rtcEpoch(check)-now))<=1;
        lcd::log("rtc ntp-write=%d verified=%d OS-clear=%d\n",written,verified,verified);
    }
}

// Settings owns the panel while open. Every other redraw routes here, and
// minute updates use CLOCK_GAP even when the underlying selection is artwork.
void closeSettings(const char* reason);
bool showSettings() {
    auto* pixels=panel.framebuffer();
    if(!pixels) { lcd::log("settings error=allocation\n"); closeSettings("error"); return false; }
    char footer[192];
    snprintf(footer,sizeof footer,"VENTANAL · %s · WI-FI %s · v1.0.0",
             VENTANAL_DEVICE_ID,WiFi.status()==WL_CONNECTED?"OK":"OFFLINE");
    lcd::drawSettingsPage(pixels,settings,solarPublished(),settingsBatteryButton(),footer);
    clockSlot=-1;
    if(clockBackground && clockPixels && clockDimmed) {
        clockSlot=1; // LCARS top-bar geometry; never the underlying art corner
        memset(clockBackground,0,204*26*2);
        renderClock();
        lcd::pasteReadout(pixels,clockPixels,lcd::CLOCK_GAP);
    }
    const bool ok = panel.blitDimmed(pixels, pixels, settings.brightness);
    lcd::log("settings draw=%d brightness=%u solarListed=%d batteryButton=%d\n",
             ok,settings.brightness,solarPublished(),settingsBatteryButton());
    return ok;
}

// Copy the resident pixels to the scan-out framebuffer and compose overlays there.
bool show(int index) {
    if(!powerSession.screenOn || powerSession.chargeShowing) return false;
    if(settingsPage.active) return showSettings();
    const int slot = screenSlot(index);
    if (slot < 0) {   // persisted index outside the current screen list
        lcd::log("blit error=screen-index index=%d\n", index);
        return false;
    }
    const bool overrideArt=slot==0 && owner.overrideArt.id[0];
    uint16_t* pixels = panel.framebuffer();
    if (!pixels) {
        lcd::log("blit error=allocation\n");
        return false;
    }
    const uint32_t started = micros();
    const uint8_t* frame = overrideArt ? overrideFrame
        : state.cachedIds[slot][0] ? residentFrames[slot] : nullptr;
    if (!frame) {
        lcd::drawWaiting(pixels, lcd::WIDTH, lcd::HEIGHT, VENTANAL_HOST_URL);
    } else {
        memcpy(pixels, frame + lcd::HEADER_BYTES, lcd::PIXEL_BYTES);
        if (state.offline) lcd::drawOfflineMark(pixels, lcd::WIDTH, lcd::HEIGHT);
    }
#ifdef VENTANAL_LCD_DIAG_PANEL
    const uint32_t backgroundUs = micros() - started;
#endif
    if (const char* text = lcd::refreshLabelText(refreshLabel.label())) {
        const bool red = refreshLabel.label() == lcd::RefreshLabel::Offline
                      || refreshLabel.label() == lcd::RefreshLabel::Failed;
        lcd::drawRefreshLabel(pixels, lcd::WIDTH, lcd::HEIGHT, text,
                              red ? lcd::RGB565_RED : lcd::RGB565_WHITE);
    }
    clockSlot = -1;
    if(frame && clockBackground && clockPixels && clockDimmed) {
        clockSlot=slot;
        const auto r=lcd::readoutRect(slot);
        for(int y=0;y<r.h;++y)
            memcpy(clockBackground+y*r.w,pixels+(r.y+y)*lcd::WIDTH+r.x,size_t(r.w)*2);
        renderClock();
        lcd::pasteReadout(pixels,clockPixels,r);
    }
    const bool ok = panel.blitDimmed(pixels, pixels, settings.brightness) && frame;
    const uint32_t elapsed = micros() - started;
    state.drawn = ok;
    if(ok) lcd::log("photo-ready slot=%s clock=board region-only-minute-redraw battery=none\n",lcd::SLOT_NAMES[slot]);
    lcd::log("blit screen=%s id=%.16s cache=%s offline=%d brightness=%u ms=%lu\n",
             lcd::SLOT_NAMES[slot], overrideArt ? owner.overrideArt.id : state.cachedIds[slot],
             ok ? "frame" : "waiting", state.offline,settings.brightness,
             static_cast<unsigned long>(elapsed / 1000));
#ifdef VENTANAL_LCD_DIAG_PANEL
    lcd::log("panel copy screen=%s background_us=%lu total_us=%lu\n", lcd::SLOT_NAMES[slot],
             static_cast<unsigned long>(backgroundUs), static_cast<unsigned long>(elapsed));
#endif
    return ok;
}

void setBacklight(bool on) {
    if (on == backlightState) return;
    backlightState = on;
    expander.setBacklight(on);
    lcd::log("backlight on=%d\n", on);
}

// Log label transitions only (captures stay quiet while nothing changes).
lcd::RefreshLabel lastLoggedLabel = lcd::RefreshLabel::None;

// Log every label transition so captures prove the feedback cycle.
void logRefreshLabel() {
    if (refreshLabel.label() == lastLoggedLabel) return;
    lastLoggedLabel = refreshLabel.label();
    const char* text = lcd::refreshLabelText(refreshLabel.label());
    lcd::log("label=%s\n", text ? text : "none");
}

// The quiet window comes from the live manifest (persisted at boot, then
// refreshed with every accepted manifest) and only applies once the wall
// clock is trustworthy: before the first NTP sync the backlight stays on.
void backlightRefresh() {
    quietBlanking = false;
    if(!powerSession.screenOn) { setBacklight(false); return; }
    if(powerSession.chargeShowing) { setBacklight(true); return; }
    tm local{};
    const time_t now = time(nullptr);
    localtime_r(&now, &local);
    const bool quiet = lcd::quietApplies(local.tm_year + 1900,
                                         local.tm_hour * 60 + local.tm_min,
                                         state.manifest);
    quietBlanking = quiet && millis() >= touchUntilMs;
    setBacklight(!quietBlanking);
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
lcd::DeviceSettings refreshSettings; // copied before worker starts; immutable until it finishes
lcd::RefreshResult refreshStaging;   // worker writes, loop copies while parked
char refreshIds[lcd::SCREEN_SLOTS][17]{};
struct PendingFrame {
    int slot;
    const lcd::FrameInfo* info;
    const lcd::FrameBody* body;
};
PendingFrame pendingFrame;
SemaphoreHandle_t frameStaged;       // worker -> loop: decoded frame ready
SemaphoreHandle_t frameCopied;       // loop -> worker: scratch and body consumed

void stageResidentFrame(int slot, const lcd::FrameInfo* info, const lcd::FrameBody& body) {
    pendingFrame = {slot, info, &body};
    xSemaphoreGive(frameStaged);
    xSemaphoreTake(frameCopied, portMAX_DELAY);
}

void clearOverrideSave() {
    deferredOverride = {};
    overrideSource = -1;
    free(overrideFrame);
    overrideFrame = nullptr;
}

void applyResidentFrame() {
    const auto frame = pendingFrame;
    auto& id = state.cachedIds[frame.slot];
    const char* nextId = frame.info ? frame.info->id : "";
    if (frame.info) std::swap(residentFrames[frame.slot], refreshCache.staging);
    const bool changed = strcmp(id, nextId) != 0;
    if (mounted && (changed || refreshCache.forceDownloads)) {
        auto& save = deferredFrames[frame.slot];
        save.replace(nextId, *frame.body, millis());
        lcd::log("cache queued slot=%s bytes=%u free_psram=%u\n", lcd::SLOT_NAMES[frame.slot],
                 unsigned(frame.body->size), unsigned(ESP.getFreePsram()));
    }
    strcpy(id, nextId);
    if (frame.slot == 0 && strcmp(owner.mainId, id) != 0) {
        owner.mainChanged(id);
        clearOverrideSave();
    }
    xSemaphoreGive(frameCopied);
    if (changed && !settingsPage.active && screenSlot(screenIndex) == frame.slot)
        show(screenIndex);
}

bool pendingCacheSave(uint32_t now) {
    if (deferredOverride.ready(now)) return true;
    for (int slot = 0; slot < lcd::SCREEN_SLOTS; ++slot)
        if (slot != overrideSource && deferredFrames[slot].ready(now)) return true;
    return false;
}

void persistDeferredFrame() {
    if (!mounted || settingsPage.active) return;
    const uint32_t now = millis();

    // The spare backing a selected override must stay available until its
    // archive is committed. Other slots can save in either order.
    const auto due = [now](const lcd::DeferredSave& save) {
        uint32_t elapsed = now - save.queuedAt;
#ifdef VENTANAL_LCD_DIAG_PANEL
        if (elapsed >= 12000) elapsed = lcd::MAX_SAVE_DELAY_MS; // exercise the deadline
#endif
        return lcd::persistNow(backlightState, quietBlanking, elapsed, save.ready(now));
    };
    int slot = -1;
    lcd::DeferredSave* chosen = due(deferredOverride) ? &deferredOverride : nullptr;
    if (!chosen) {
        for (int i = 0; i < lcd::SCREEN_SLOTS; ++i) {
            if (i == overrideSource || !due(deferredFrames[i])) continue;
            if (!chosen || now - deferredFrames[i].queuedAt > now - chosen->queuedAt) {
                chosen = &deferredFrames[i];
                slot = i;
            }
        }
    }
    if (!chosen) return;

    const bool wasLit = backlightState;
    if (wasLit) setBacklight(false);
    const uint32_t started = millis();
    const size_t bytes = chosen->body.size;
    bool ok;
    if (slot < 0) {
        if (chosen->body.bytes) ok = lcd::writeCachedFrame("/override.vnr", chosen->body);
        else {
            const String source = residentPath(overrideSource, persistedFrames[overrideSource].alternate);
            ok = lcd::copyCachedFrame("/override.vnr", source.c_str());
        }
        if (ok) overrideSource = -1;
    } else {
        lcd::CacheLocation next;
        strcpy(next.id, chosen->id);
        next.alternate = !persistedFrames[slot].alternate;
        const String path = residentPath(slot, next.alternate);
        ok = (!chosen->body.bytes || lcd::writeCachedFrame(path.c_str(), chosen->body))
            && lcd::saveCacheLocation(prefs, slot, next);
        if (ok) {
            persistedFrames[slot] = next;
            if (slot == 0) lcd::saveOwner(prefs, owner);
        }
    }
    restartScanout("frame-persist");
    lcd::log("cache persist slot=%s id=%.16s bytes=%u ok=%d ms=%lu wait_ms=%lu lit=%d\n",
        slot < 0 ? "override" : lcd::SLOT_NAMES[slot], chosen->id, unsigned(bytes), ok,
        static_cast<unsigned long>(millis() - started),
        static_cast<unsigned long>(started - chosen->queuedAt), wasLit);
    if (ok) *chosen = {};
    else {
        chosen->failedAt = millis();
        chosen->retrying = true;
    }
    if (wasLit) {
        delay(30); // allow the queued restart to reach VSYNC before lighting the panel
        backlightRefresh();
    }
}

void refreshWorker(void*) {
    refreshWatchdog.subscribe();
    lcd::runRefresh(VENTANAL_HOST_URL, VENTANAL_DEVICE_ID, refreshIds,
                    refreshStaging, refreshWatchdog, refreshSettings, refreshSkips,
                    refreshCache);
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
    memcpy(refreshIds, state.cachedIds, sizeof refreshIds);
#ifdef VENTANAL_LCD_DIAG_PANEL
    refreshCache.forceDownloads = fetchCounter == 0;
#endif
    refreshSettings = settings;
    owner.queue.query(refreshSkips,sizeof refreshSkips);
    sentSkipRevision=skipRevision;
    if(owner.queue.count) lcd::log("skip reports sending count=%u\n",unsigned(owner.queue.count));
    refreshRequested = redraw;
    lastFetchMs = millis();
    refreshBusy = true;
    if (xTaskCreate(refreshWorker, "ventanal-refresh", WORKER_STACK_BYTES,
                    nullptr, 1, nullptr) != pdPASS) {
        refreshBusy = false;
        lcd::log("refresh error=task-create\n");
        return false;
    }
    settingsRefreshPending=false;
    return true;
}

// Apply a finished worker pass (single writer: loop()).
void applyFinishedRefresh() {
    const int previousIndex = screenIndex;
    // Did anything the screen shows change? frameOk slots that merely
    // re-stage the current id (skipped=unchanged) are not new content,
    // and a drop only counts when the slot had something to lose: solar
    // (and unpublished spares) are re-staged as drops on every pass.
    bool changed = false;
    if (refreshStaging.manifestOk) {
        for (int slot = 0; slot < lcd::SCREEN_SLOTS; ++slot) {
            if ((refreshStaging.dropSlot[slot] && refreshIds[slot][0])
                || (refreshStaging.frameOk[slot]
                    && strcmp(refreshStaging.frameIds[slot],
                              refreshIds[slot]))) {
                changed = true;
                break;
            }
        }
    }
    const bool redraw = lcd::applyRefresh(state, screenIndex, spareCursor,
                                          prefs, refreshStaging,
                                          refreshRequested,settings);
    if(refreshStaging.manifestOk) {
        const unsigned before=owner.queue.count;
        lcd::acknowledgeSkips(owner,skipRevision-sentSkipRevision,true);
        lcd::log("skip reports acknowledged count=%u pending=%u\n",before-owner.queue.count,unsigned(owner.queue.count));
        if (before != owner.queue.count) lcd::saveOwner(prefs,owner);
        restartScanout("refresh-nvs");
    }
    xSemaphoreGive(refreshCopied);
    refreshBusy = false;
    // A manifest that arrived but lost a frame it required is a failure for
    // feedback: DOWNLOAD FAILED (red), never UP TO DATE over stale artwork.
    const bool frameFailed = lcd::anyRequiredFrameFailed(refreshStaging);
    if (refreshRequested)
        refreshLabel.finish(millis(), !refreshStaging.manifestOk, changed,
                            frameFailed);
    else if (redraw)
        refreshLabel.clear();   // a scheduled redraw replaces the held label
    logRefreshLabel();
    lcd::log("refresh fetch=%lu ok=%d offline=%d failed=%d\n", fetchCounter++,
             refreshStaging.manifestOk, state.offline, frameFailed);
    if (screenIndex != previousIndex)   // normalised to a shrunk list
        lcd::log("screen=%s\n", lcd::SLOT_NAMES[screenSlot(screenIndex)]);
    if (redraw) show(screenIndex);
    else if (refreshLabel.label() != lcd::RefreshLabel::None)
        show(screenIndex);   // redraw the cached frame with the label
    backlightRefresh();
}

// Task 6 calls this from the UI thread when a settings choice changes.
// An in-flight worker finishes its immutable snapshot; the next pass uses the
// new visibility so newly enabled screens get downloaded even after a toggle.
bool applyDeviceSettings(const lcd::DeviceSettings& next) {
    if(!lcd::validSettings(next)) return false;
    const int previous=screenSlot(screenIndex);
    const bool visibilityChanged=next.showArt!=settings.showArt || next.showWeather!=settings.showWeather
        || next.showSolar!=settings.showSolar;
    const bool saved = lcd::saveSettings(settingsPrefs, next);
    restartScanout("settings");
    if (!saved) return false;
    settings=next;
    lcd::remapSelection(screenIndex,spareCursor,previous,solarPublished(),spareCount(),settings);
    storeScreenIndex();
    show(screenIndex);
    if(visibilityChanged) settingsRefreshPending=true;
    return true;
}

// --- Touch ----------------------------------------------------------------

void closeSettings(const char* reason) {
    settingsPage.active=false;
    touchPoller.controlsMode(false);
    lcd::remapSelection(screenIndex,spareCursor,settingsReturnSlot,solarPublished(),spareCount(),settings);
    storeScreenIndex();
    lcd::log("settings close=%s return=%s\n",reason,lcd::SLOT_NAMES[screenSlot(screenIndex)]);
    show(screenIndex);
}
void settingsTap(int x,int y) {
    const auto hit=lcd::hitSettings(x,y,solarPublished(),settingsBatteryButton());
    lcd::log("settings hit=%d x=%d y=%d\n",int(hit),x,y);
    if(hit==lcd::SettingsHit::Done) { closeSettings("done"); return; }
    auto next=settings;
    const bool accepted=lcd::applySettingsHit(next,hit,solarPublished(),settingsBatteryMv());
    if(accepted && applyDeviceSettings(next)) {
        lcd::log("settings change=%d brightness=%u timeout=%u art=%d weather=%d solar=%d saved=1\n",
                 int(hit),settings.brightness,settings.timeoutMin,settings.showArt,settings.showWeather,settings.showSolar);
    } else lcd::log("settings change=%d accepted=0\n",int(hit));
}

void handleGesture(Gesture gesture, int x, int y) {
    if(settingsPage.active) {
        if(gesture==Gesture::Next || gesture==Gesture::Previous) settingsTap(x,y);
        return;
    }
    if((gesture==Gesture::Next || gesture==Gesture::Previous)
       && classifyTap(x,y,lcd::isLcarsSlot(screenSlot(screenIndex)))==TapTarget::OpenSettings) {
        settingsReturnSlot=screenSlot(screenIndex);
        settingsPage.open(millis());
        powerSession.activity(millis());
        touchPoller.controlsMode(true);
        refreshLabel.clear();
        lcd::log("settings open return=%s x=%d y=%d\n",lcd::SLOT_NAMES[settingsReturnSlot],x,y);
        if(!showSettings() && settingsPage.active) closeSettings("error");
        return;
    }
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
        {
            if (!mounted) { lcd::log("spare skipped=littlefs-unmounted\n"); break; }
            if(!settings.showArt) { lcd::log("spare skipped=art-hidden\n"); break; }
            bool cached[3];
            for(int i=0;i<3;++i) {
                strcpy(owner.spares[i].id,state.cachedIds[3+i]);
                cached[i]=owner.spares[i].id[0];
            }
            char skipped[17]; strcpy(skipped,lcd::selectedArtId(owner));
            const bool swapped=lcd::swapArt(owner,settings.showArt,cached,[](int next) {
                // Swaps and frame publication both run in loop(). Any tap
                // queued during a hand-off is classified before it gets here.
                if (!overrideFrame)
                    overrideFrame = static_cast<uint8_t*>(ps_malloc(lcd::FRAME_BYTES));
                if (!overrideFrame) return false;
                memcpy(overrideFrame, residentFrames[3+next], lcd::FRAME_BYTES);
                return true;
            });
            if (!swapped) {
                lcd::log("spare skipped=no-psram\n");
                break;
            }
            if (owner.cursor < 0) clearOverrideSave();
            else {
                const int slot = 3 + owner.cursor;
                const auto& save = deferredFrames[slot];
                const bool inRam = save.pending && save.body.bytes;
                overrideSource = inRam ? -1 : slot;
                deferredOverride.replace(owner.overrideArt.id, inRam ? save.body : lcd::FrameBody{}, millis());
            }
            if(strlen(skipped)==16 && strlen(lcd::selectedArtId(owner))==16
               && strcmp(skipped,lcd::selectedArtId(owner))) ++skipRevision;
            spareCursor=owner.cursor;
            screenIndex=0;
            lcd::saveOwner(prefs,owner);
            restartScanout("skip-queue");
            lcd::log("spare selected=%d id=%s queued=%u screen=art\n",owner.cursor,lcd::selectedArtId(owner),unsigned(owner.queue.count));
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

void restoreTouchWake();
void pollTouch(uint32_t now) {
    if(!powerSession.touchReady) return;
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
    if(touched && !powerSession.screenOn) {
        restoreTouchWake(); // a contact during a background download also wakes
        return;
    }
    if(touched) powerSession.activity(now);
    if(touched && settingsPage.active) settingsPage.touch(now);
    if (touched && !lastTouched) touchUntilMs = now + TOUCH_WAKE_MS;
    lastTouched = touched;
    if (gesture != Gesture::None && !powerSession.chargeShowing) handleGesture(gesture, touchPoller.pressX(), touchPoller.pressY());
}

// Never call touch.begin() here: that would drive INT and select an address.
bool probeTouchAfterWake() {
    Wire.beginTransmission(Gt911::PRIMARY);
    const bool answers=Wire.endTransmission()==0;
    uint8_t product[3]={};
    const bool ok=answers && touch.address()==Gt911::PRIMARY
        && touch.peek(0x8140,product,sizeof product)
        && product[0]=='9' && product[1]=='1' && product[2]=='1';
    lcd::log(ok?"gt911 ok after wake\n":"gt911 error after wake\n");
    return ok;
}
void showChargeMe() {
    if(!panel.begin(setLcdResetLine)) {
        lcd::log("power error=panel-resume\n");
        powerSession.chargeFailed(millis());
        setBacklight(false);
        return;
    }
    auto* pixels=panel.framebuffer();
    if(pixels) {
        lcd::drawSettingsChrome(pixels,false);
        lcd::drawText(pixels,lcd::WIDTH,lcd::HEIGHT,StatusFont,
                      "CHARGE ME",280,240,lcd::rgb565(221,68,68));
        panel.blitDimmed(pixels, pixels, settings.brightness);
    } else lcd::log("power error=charge-allocation\n");
    clockSlot=-1;
    powerSession.showCharge(millis());
    setBacklight(true);
    lcd::log("power charge-me seconds=10\n");
}
void restoreTouchWake() {
    powerSession.wakeTouch(millis());
    touchUntilMs=millis()+TOUCH_WAKE_MS;
    lastTouched=false;
    touchPoller.suppressWakeTouch();
    refreshLabel.clear(); logRefreshLabel();
    if(powerLevel()==lcd::BatteryLevel::Critical) showChargeMe();
    else if(panel.begin(setLcdResetLine)) {
        show(screenIndex); // cache + current clock, dimmed, before network I/O
        backlightRefresh();
        lcd::log("power restored brightness=%u screen=%s label=idle\n",
                 settings.brightness,lcd::SLOT_NAMES[screenSlot(screenIndex)]);
    } else lcd::log("power error=panel-resume\n");
}
void sleepPower() {
    setBacklight(false);
    // Let loop() drain one file per pass before entering light sleep.
    if (!settingsPage.active && pendingCacheSave(millis())) {
        powerSession.sleep();
        return;
    }
    if(!panel.stop()) { lcd::log("power error=panel-stop\n"); delay(1000); return; }
    powerSession.sleep();
    // Manual light sleep cannot preserve association. The existing worker
    // reconnects on demand, using its bounded/watchdog-supervised path.
    WiFi.mode(WIFI_OFF);
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
    uint32_t wait=lcd::pollWaitMs(millis()-lastFetchMs,powerPollMs());
#ifdef VENTANAL_LCD_DIAG_POWER
    if(powerLevel()!=lcd::BatteryLevel::Critical) wait=20000; // capture timer wake
#endif
    esp_err_t armed=gpio_wakeup_enable(GPIO_NUM_4,GPIO_INTR_LOW_LEVEL);
    if(armed==ESP_OK) armed=esp_sleep_enable_gpio_wakeup();
    if(armed==ESP_OK && wait) armed=esp_sleep_enable_timer_wakeup(uint64_t(wait)*1000);
    lcd::log("power sleep timer_ms=%lu critical=%d int=%d\n",
             static_cast<unsigned long>(wait),powerLevel()==lcd::BatteryLevel::Critical,digitalRead(4));
    Serial.flush();
    const int result=lcd::sleepWithUsb(int(armed),[] { return int(esp_light_sleep_start()); },
        [](bool enabled) {
#if ARDUINO_USB_CDC_ON_BOOT && ARDUINO_USB_MODE && (defined(VENTANAL_LCD_DIAG_POWER) || defined(VENTANAL_LCD_USB_SLEEP_REENUM))
            // Experimental: hardware capture did not recover CDC on this host.
            usb_serial_jtag_ll_phy_enable_pad(enabled);
            if(!enabled) delay(20); // let the host observe detach before clocks stop
#else
            (void)enabled;
#endif
        });
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
    gpio_wakeup_disable(GPIO_NUM_4);
    if(result!=ESP_OK) {
        lcd::log("power error=sleep code=%d\n",int(result));
        // Bounded retry while dark; do not claim a successful wake.
        delay(1000);
        return;
    }
    // A rejected/failed sleep has no new wake cause or post-wake probe.
    const auto reason=esp_sleep_get_wakeup_cause();
    if(powerSession.touchReady || powerSession.probeDue(millis()))
        powerSession.probed(probeTouchAfterWake(),millis());
    if(reason==ESP_SLEEP_WAKEUP_GPIO) {
        lcd::log("power wake reason=touch\n");
        restoreTouchWake();
    } else if(reason==ESP_SLEEP_WAKEUP_TIMER) {
        lcd::log("power wake reason=timer\n");
        powerSession.wakeTimer();
    } else lcd::log("power wake reason=other code=%d\n",int(reason));
}
void tickPower() {
    if(powerSession.probeDue(millis()))
        powerSession.probed(probeTouchAfterWake(),millis());
    // Quiet hours are driven by backlightRefresh(), including touch override.
    const auto action=lcd::decidePower({false,false,powerLevel(),powerSession.idle(millis()),
                                       settings.timeoutMin,false});
    switch(powerSession.step(action,millis(),refreshBusy)) {
    case lcd::PowerStep::ShowCharge: showChargeMe(); break;
    case lcd::PowerStep::Restore: show(screenIndex); backlightRefresh(); break;
    case lcd::PowerStep::Sleep: sleepPower(); break;
    case lcd::PowerStep::None: break;
    }
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
    mounted = LittleFS.begin(true, "/littlefs", 10, "littlefs");
    if (!mounted) lcd::log("error=littlefs-mount\n");
    // Restore persisted state so an offline boot still honours quiet hours
    // and the screen list matches the cache before the first fetch.
    lcd::loadCacheLocations(prefs, persistedFrames);
    lcd::loadOwner(prefs,owner);
    if(!owner.mainId[0]) owner.mainChanged(persistedFrames[0].id);
    lcd::loadSelection(prefs, screenIndex, spareCursor);
    lcd::loadQuiet(prefs, state.manifest);
    lcd::loadPublishShape(prefs, state.manifest);
    // Saved indices refer to the persisted shape and persisted visibility.
    lcd::loadSettings(settingsPrefs,settings);
    int slots[lcd::SCREEN_SLOTS];
    const int count=lcd::activeSlots(solarPublished(),spareCount(),settings,slots,lcd::SCREEN_SLOTS);
    const int previous=screenIndex>=0 && screenIndex<count ? slots[screenIndex] : -1;
    lcd::remapSelection(screenIndex,spareCursor,previous,solarPublished(),spareCount(),settings);
#ifdef VENTANAL_LCD_DIAG_SETTINGS
    auto diagnostic=settings; diagnostic.brightness=50; diagnostic.showWeather=false;
    settings=diagnostic; // deliberately RAM-only: normal build restores owner settings
    lcd::remapSelection(screenIndex,spareCursor,previous,solarPublished(),spareCount(),settings);
#endif
#ifdef VENTANAL_LCD_DIAG_POWER
    settings.timeoutMin=1; settings.brightness=25; // RAM only; normal build restores NVS
    lcd::log("diag power timeout_min=1 brightness=25 sleep_timer_ms=20000 RAM-only\n");
#endif
    lcd::log("settings brightness=%u timeout=%u art=%d weather=%d solar=%d\n",
             settings.brightness,settings.timeoutMin,settings.showArt,settings.showWeather,settings.showSolar);
    // Initialise the configured timezone BEFORE any refresh (and independent
    // of connectivity): an offline reboot with retained RTC time evaluates
    // the persisted LOCAL quiet window in the configured zone, not UTC.
    lcd::configureTimezone();
    if (!expander.begin()) lcd::log("error=ch422g-begin\n");
    expander.setBacklight(false);
    if (!panel.begin(setLcdResetLine)) lcd::log("error=panel-begin\n");
#ifdef VENTANAL_LCD_DIAG_RESTORE_ART
    if (!settings.showArt) {
        settings.showArt = lcd::defaultSettings(lcd::POWER_SOURCE_DETECTABLE).showArt;
        const bool saved = lcd::saveSettings(settingsPrefs, settings);
        restartScanout("restore-art");
        lcd::remapSelection(screenIndex, spareCursor, previous, solarPublished(), spareCount(), settings);
        lcd::log("diag restore art=%d saved=%d\n", settings.showArt, saved);
    }
#endif
#ifdef VENTANAL_LCD_DIAG_PANEL
    settings.timeoutMin=0; // RAM-only: keep VSYNC stress observation alive
#endif
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
    {   // Observe scan-out while refreshing and saving cached frames.
        esp_lcd_rgb_panel_event_callbacks_t callbacks = {};
        callbacks.on_vsync = panelVsync;
        if (esp_lcd_rgb_panel_register_event_callbacks(panel.handle(),
                                                       &callbacks, nullptr)
            != ESP_OK)
            lcd::log("diag panel error=vsync-callback\n");
    }
#endif
#ifdef VENTANAL_LCD_DIAG_HW
    scanBus();
    expander.setBacklight(true);
    lcd::hwProbeBegin();
    return;  // no network worker or private configuration values in probe logs
#endif
    sntp_set_time_sync_notification_cb(onNtpSync);
    seedRtc(); // after GT911 reset sequence; never reconfigure the shared bus
    loadResidentCache();
    WiFi.persistent(false);
    refreshWatchdog.arm();
    refreshStaged = xSemaphoreCreateBinary();
    refreshCopied = xSemaphoreCreateBinary();
    frameStaged = xSemaphoreCreateBinary();
    frameCopied = xSemaphoreCreateBinary();
    refreshCache.publish = stageResidentFrame;
    powerSession.activity(millis());
    show(screenIndex);            // cached frame, or the waiting screen
#ifdef VENTANAL_LCD_DIAG_PANEL
    stressStartMs = vsyncReportMs = millis();
    vsyncReportFrames = vsyncFrames;
    vsyncMaxGapMs = 0;
    lcd::log("diag panel stress=start cycles=90 interval_ms=2000 force_first_refresh=1\n");
#endif
    startRefresh(true);
#ifdef VENTANAL_LCD_DIAG_SETTINGS_PAGE
    // Capture-only flag: exercise the normal Settings open route without
    // changing or persisting any owner setting. Normal builds omit it.
    handleGesture(Gesture::Next,600,20);
#endif
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
#ifdef VENTANAL_LCD_DIAG_HW
    lcd::hwProbeTick(now, expander);
    delay(2);
    return;
#endif
    if(ntpRtcPending.exchange(false)) saveRtcAfterNtp();
    if (refreshBusy && xSemaphoreTake(frameStaged, 0) == pdTRUE) applyResidentFrame();
    tickClock();
    pollTouch(now);
    if(settingsPage.expired(millis())) closeSettings("idle");
    backlightRefresh();
    // The label hold (~2 s) expires here: redraw the cached frame clean.
    if (refreshLabel.tick(now)) {
        logRefreshLabel();
        show(screenIndex);
    }
#ifdef VENTANAL_LCD_DIAG_PANEL
    panelVsyncReport(now);
    if (stressCycles < 90 && now - stressStartMs >= (stressCycles + 1) * 2000u) {
        ++stressCycles;
        screenIndex = (screenIndex + 1) % screenCount();
        lcd::log("diag panel cycle=%u elapsed_ms=%lu screen=%s refresh_busy=%d\n",
                 stressCycles, static_cast<unsigned long>(now - stressStartMs),
                 lcd::SLOT_NAMES[screenSlot(screenIndex)], refreshBusy);
        show(screenIndex);
        if (stressCycles == 90) lcd::log("diag panel stress=done\n");
    }
#endif
    if (refreshBusy && xSemaphoreTake(refreshStaged, 0) == pdTRUE)
        applyFinishedRefresh();
    const uint32_t interval=powerPollMs();
    if(powerSession.refreshDue(now,lastFetchMs,interval,powerLevel(),refreshBusy,settingsRefreshPending)) {
        const bool started=startRefresh(false);
        powerSession.refreshStarted(started);
        if(started) settingsRefreshPending=false;
    }
    persistDeferredFrame();
    tickPower();
    delay(2);
}
