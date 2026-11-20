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
// run. The loop owns display state and cache saves. The worker reserves
// replay counters for paired requests and signals the loop after writing.

#include "../common/release_key.h"
#include <Arduino.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <Wire.h>
#include <WiFi.h>
#include "wifi_retry.h"
#include <esp_task_wdt.h>
#include <esp_sleep.h>
#include <esp_heap_caps.h>
#include <esp_app_desc.h>
#include <esp_system.h>
#include <esp_ota_ops.h>
#include <sdkconfig.h>
#if ARDUINO_USB_CDC_ON_BOOT && ARDUINO_USB_MODE
#include <hal/usb_serial_jtag_ll.h>
#endif
#include <driver/gpio.h>
#include <algorithm>
#include <cstdio>
#include <ctime>
#include <sys/time.h>
#include <atomic>
#include "clock.h"

#include "build_config.h"
#include "installation.h"
#include "ota.h"

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
#include "display_owner.h"
#include "persist.h"
#include "transport.h"
#include "owner.h"
#include "screens.h"
#include "settings_page.h"
#include "setup_record.h"
#include "setup_screen.h"
#include "../common/setup_portal/locate.h"
#include <esp_mac.h>
#include <esp_random.h>
#include <qrcode.h>
#include <memory>
#include <string>
#include "sync.h"
#include "local_provider.h"
#include "local_media.h"
#include "touch.h"
#include <esp_sntp.h>

#if defined(PLATFORMIO) && (!defined(CONFIG_SPIRAM_XIP_FROM_PSRAM) || \
    !defined(CONFIG_SPIRAM_FETCH_INSTRUCTIONS) || !defined(CONFIG_SPIRAM_RODATA) || \
    CONFIG_ESP32S3_DATA_CACHE_LINE_SIZE != 64)
#error The LCD build requires PSRAM XIP and a 64-byte data cache line.
#endif
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

// Installation owns the loop while the panel is blanked. Give its TLS
// connection the same stack budget as the refresh worker.
SET_LOOP_TASK_STACK_SIZE(16 * 1024);

namespace {
#include "app_state.h"
#include "app_diag.h"
#include "app_panel.h"
#include "app_buffers.h"
#include "app_cache_load.h"
#include "app_clock.h"
#include "app_display.h"
#include "app_refresh.h"
#include "app_update.h"
#include "app_input.h"
#include "app_setup.h"
#include "app_power.h"
#include "app_diag_probes.h"

}  // namespace

void setup() {
    WiFi.persistent(false);  // keep the Wi-Fi driver out of the small shared NVS partition
    const bool pendingUpdate = lcd::armUpdateDeadline();
#if ARDUINO_USB_CDC_ON_BOOT && ARDUINO_USB_MODE
    // HWCDC (native USB-Serial/JTAG): keep the last boot lines while no
    // host has the port open (the not-connected write path drops only the
    // oldest queued bytes), and bound every write even if a host stalls.
    Serial.setTxBufferSize(4096);
    Serial.setTxTimeoutMs(20);
#endif
    Serial.begin(115200);
    delay(300);
    const auto* slot = esp_ota_get_running_partition();
    lcd::log("ventanal lcd43b boot fw=%s app=%s slot=%s\n", lcd::FIRMWARE_VERSION,
             esp_app_get_description()->version, slot ? slot->label : "unknown");
    if (VENTANAL_TEST_RELEASE_KEY)
        lcd::log("!!! TEST RELEASE KEY - BENCH FIRMWARE ONLY !!!\n");
    lcd::log("usb cdc hw=%d onboot=%d\n", ARDUINO_USB_MODE,
             ARDUINO_USB_CDC_ON_BOOT);
    lcd::log("memory psram_bytes=%u xip=%d dcache_line_bytes=%d\n",
             unsigned(ESP.getPsramSize()),
#ifdef CONFIG_SPIRAM_XIP_FROM_PSRAM
             1,
#else
             0,
#endif
             CONFIG_ESP32S3_DATA_CACHE_LINE_SIZE);
    Wire.begin(8, 9);   // SDA=GPIO8, SCL=GPIO9, shared by expander and touch
    Wire.setClock(400000);
    if (!expander.begin()) lcd::log("error=ch422g-begin\n");
    backlightRefresh();  // panel not started yet: the owner reports Off
    allocateLocalPages();
    installationReady = lcd::initializeInstallation(pendingUpdate);
    if (installationReady && !prefs.begin("ventanal", false)) lcd::log("error=nvs-open\n");
    updateBoot = lcd::readUpdateBoot(prefs);
    int previous = -1;
    if (installationReady) {
        mounted = lcd::mountUpdateCache(prefs);
        if (!mounted) lcd::log("error=littlefs-mount\n");
        else lcd::sweepCacheTemps();
        // Restore persisted state so an offline boot still honours quiet hours
        // and the screen list matches the cache before the first fetch.
        lcd::loadCacheLocations(prefs, persistedFrames);
        lcd::loadOwner(prefs,owner);
        if(!owner.mainId[0]) owner.mainChanged(persistedFrames[0].id);
        lcd::loadSelection(prefs, screenIndex, spareCursor);
        selectionSave.restored(screenIndex, spareCursor);
        lcd::loadQuiet(prefs, state.manifest);
        lcd::loadPublishShape(prefs, state.manifest);
        // Saved indices refer to the persisted shape and persisted visibility.
        lcd::loadSettings(settingsPrefs,settings);
        int slots[lcd::SCREEN_SLOTS];
        const int count=lcd::activeSlots(solarPublished(),spareCount(),settings,slots,lcd::SCREEN_SLOTS);
        previous=screenIndex>=0 && screenIndex<count ? slots[screenIndex] : -1;
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
        githubLastCheck = lcd::loadGithubCheck(prefs, githubOffer);
        if (lcd::standalone(lcd::installation))
            githubLastAutoFailure = lcd::loadGithubAutoFailure(prefs);
    }

#ifdef VENTANAL_LCD_DIAG_RESTORE_ART
    if (installationReady && !settings.showArt) {
        settings.showArt = lcd::defaultSettings(lcd::POWER_SOURCE_DETECTABLE).showArt;
        const bool saved = lcd::saveSettings(settingsPrefs, settings);
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
#ifdef VENTANAL_LCD_DIAG_HW
    beginPanel();
    scanBus();
    backlightRefresh();
    lcd::hwProbeBegin();
    return;  // no network worker or private configuration values in probe logs
#endif
    if (!installationReady) {
        if (updateBoot.pending) lcd::rollbackUpdate();
        if (lcd::bootSetup(lcd::validInstallation(lcd::installation), installationReady)
                == lcd::BootSetup::FirstBoot && enterSetup(lcd::SetupReason::FirstBoot)) {
            backlightRefresh();
            return;
        }
        show(screenIndex);
        backlightRefresh();
        return;
    }
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
    refreshCache.manifestVerified = [] { updateManifestVerified.store(true); };
    refreshCache.reclaimWireMemory = requestWireMemory;
    refreshCache.releaseFrame = requestReleaseFrame;
    powerSession.activity(millis());
    show(screenIndex);            // cached frame, or the waiting screen
#ifdef VENTANAL_LCD_DIAG_PANEL
#ifdef VENTANAL_LCD_DIAG_F3
#ifdef VENTANAL_LCD_DIAG_DIM
    stressCycles = 90;
    stressSettingsCycles = 20;
    const auto reset = esp_reset_reason();
    if (reset == ESP_RST_INT_WDT || reset == ESP_RST_PANIC) {
        dimStep = 101;
        lcd::log("diag dim=aborted after watchdog or panic\n");
    } else if (!Panel::verifyDimming()) {
        dimStep = 101;
        lcd::log("diag dim=aborted RGB565 reference mismatch\n");
    } else {
        lcd::log("diag dim=start RGB565 exhaustive=pass\n");
    }
#elif defined(VENTANAL_LCD_DIAG_WRITE_ONLY)
    stressProfile = 2;
    stressCycles = 90;
    stressSettingsCycles = 20;
    lcd::log("diag write-only=start\n");
#else
    lcd::log("diag matrix profile=0 level=default split=0\n");
#endif
#endif
    stressStartMs = vsyncReportMs = millis();
    touchUntilMs = stressStartMs + 182000;
    vsyncReportFrames = vsyncFrames.load(std::memory_order_relaxed);
    vsyncMaxGapMs.store(0, std::memory_order_relaxed);
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
    if (!installationReady) {
        if (setupActive) {
            setupPoll(millis());
            backlightRefresh();
            delay(2);
        } else {
            delay(100);
        }
        return;
    }
    pollTouch(now);
    setupPoll(millis());  // a tap above may just have started setup (after a scan)
#ifdef VENTANAL_LCD_DIAG_SETUP_CYCLE
    diagSetupCycleTick(millis());
#endif
#ifdef VENTANAL_LCD_DIAG_SETUP_VARIANTS
    diagSetupVariantsTick(millis());
#endif
    reportBootScanout(now);
    if (lcd::pairingCounterFlash.exchange(false) && !LitNvsWrites) restartScanout("pairing-counter");
    if(ntpRtcPending.exchange(false)) saveRtcAfterNtp();
    if (refreshBusy && xSemaphoreTake(frameStaged, 0) == pdTRUE) applyResidentFrame();
    tickClock();
    if(installQueued == lcd::UpdateSource::None && settingsPage.expired(millis())) closeSettings("idle");
    backlightRefresh();
    // The label hold (~2 s) expires here: redraw the cached frame clean.
    if (refreshLabel.tick(now)) {
        logRefreshLabel();
        show(screenIndex);
    }
#ifdef VENTANAL_LCD_DIAG_PANEL
    panelVsyncReport(now);
    panel.reportBounce(now);
#ifdef VENTANAL_LCD_DIAG_DIM
    if (dimStep < 100 && now - stressStartMs >= (dimStep + 1) * 600u) {
        switch (dimStep % 5) {
        case 0:
            if (!refreshBusy) startRefresh(true);
            handleGesture(Gesture::Next, 600, 20);
            break;
        case 1:
            handleGesture(Gesture::Next, 590, 108); // 75% Settings
            break;
        case 2:
            handleGesture(Gesture::Next, 690, 410); // 75% resident page
            break;
        case 3:
            handleGesture(Gesture::Next, 600, 20);
            break;
        case 4:
            handleGesture(Gesture::Next, 720, 108); // restore 100%
            handleGesture(Gesture::Next, 690, 410);
            lcd::log("diag dim cycle=%u\n", dimStep / 5 + 1);
            break;
        }
        if (++dimStep == 100) {
            const auto health = panel.health();
            lcd::log("diag dim=done restart=%u short=%u peak_us=%u gap_us=%u\n",
                     unsigned(health.lateRestarts), unsigned(health.shortFrames),
                     unsigned(health.maxFillUs), unsigned(health.maxVsyncGapUs));
        }
    }
#endif
#ifdef VENTANAL_LCD_DIAG_F3
    if (stressSettingsCycles < 20) {
        const uint32_t base = 2000 * (stressSettingsCycles + 1);
        if (!stressSettingsOpen && now - stressStartMs >= base + 250) {
            if (!refreshBusy) startRefresh(true);
            handleGesture(Gesture::Next, 600, 20);
            stressSettingsOpen = settingsPage.active;
            lcd::log("diag settings open=%u active=%d refresh_busy=%d\n",
                     stressSettingsCycles + 1, stressSettingsOpen, refreshBusy);
        } else if (stressSettingsOpen && now - stressStartMs >= base + 1250) {
            handleGesture(Gesture::Next, 690, 410);
            stressSettingsOpen = false;
            ++stressSettingsCycles;
            lcd::log("diag settings close=%u active=%d\n",
                     stressSettingsCycles, settingsPage.active);
        }
    }
#endif
    if (stressCycles < 90 && now - stressStartMs >= (stressCycles + 1) * 2000u) {
        ++stressCycles;
        lcd::log("diag panel cycle=%u elapsed_ms=%lu screen=%s refresh_busy=%d\n",
                 stressCycles, static_cast<unsigned long>(now - stressStartMs),
                 lcd::SLOT_NAMES[screenSlot((screenIndex + 1) % screenCount())], refreshBusy);
        selectionSave.activity(now);
        handleGesture(Gesture::Next, lcd::WIDTH - 1, lcd::HEIGHT / 2);
        if (stressCycles == 90) {
            const auto health = panel.health();
            lcd::log("diag totals restart=%u short=%u peak_us=%u max_gap_us=%u\n",
                     unsigned(health.lateRestarts), unsigned(health.shortFrames),
                     unsigned(health.maxFillUs), unsigned(health.maxVsyncGapUs));
            lcd::log("diag panel stress=done max_gap_ms=%lu\n",
                     static_cast<unsigned long>(vsyncMaxGapMs.load(std::memory_order_relaxed)));
#ifdef VENTANAL_LCD_DIAG_F3
            stressAdvance = true;
#endif
        }
    }
#ifdef VENTANAL_LCD_DIAG_F3
    if (stressAdvance && !refreshBusy) {
        stressAdvance = false;
        if (stressProfile == 2) {
            lcd::log("diag matrix=done\n");
        } else {
            bool restarted = false;
            {
                HeldDark restart(lcd::DarkReason::Diag);
                if (!stopPanel()) {
                    lcd::log("diag matrix=failed panel-stop\n");
                    ESP.restart();
                }
                ++stressProfile;
                scanOtherCore = stressProfile == 2;
                Panel::setInterruptLevel(3);
                lcd::log("diag matrix profile=%u level=3 split=%d\n", stressProfile, scanOtherCore);
                restarted = beginPanel();
                if (restarted) show(screenIndex);
            }
            if (restarted) {
                backlightRefresh();
                stressCycles = stressSettingsCycles = 0;
                stressStartMs = millis();
                touchUntilMs = stressStartMs + 182000;
                vsyncLastMs = 0;
                vsyncMaxGapMs.store(0, std::memory_order_relaxed);
                startRefresh(true);
            } else lcd::log("diag matrix=failed panel-begin\n");
        }
    }
#endif
#endif
    if (refreshBusy && xSemaphoreTake(refreshStaged, 0) == pdTRUE)
        applyFinishedRefresh();
    tickUpdateBoot();
    runQueuedUpdate();
#ifdef VENTANAL_LCD_DIAG_F3
    probeLitWrites(now);
#ifdef VENTANAL_LCD_DIAG_INSTALL_VERSION
    probeBusyInstall();
#endif
#endif
    const uint32_t interval=powerPollMs();
    const bool networkIdle = !refreshBusy && installQueued == lcd::UpdateSource::None;
    const bool refreshDue = wifiRetry.waiting()
        ? networkIdle && wifiRetry.due(millis())
        : powerSession.refreshDue(now, lastFetchMs, interval, powerLevel(), !networkIdle,
                                  settingsRefreshPending);
    if (refreshDue) {
        const bool started=startRefresh(false);
        powerSession.refreshStarted(started);
        if(started) settingsRefreshPending=false;
    }
    static uint32_t settingsSaveFailedAt = 0;
    static bool settingsSaveFailed = false;
    if (settingsDirty && (!backlightState || (LitNvsWrites && !settingsPage.active))
        && (!settingsSaveFailed || now - settingsSaveFailedAt >= 5000)) {
        settingsDirty = !lcd::saveSettings(settingsPrefs, settings);
        settingsSaveFailed = settingsDirty;
        settingsSaveFailedAt = millis();
    }
    persistDeferred(false);
    if (!updateBoot.pending && installQueued == lcd::UpdateSource::None) tickPower();
    delay(2);
}
