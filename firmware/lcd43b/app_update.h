// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
//
// Firmware update controller: boot confirmation, Updating screen, install.
//
// Part of lcd43b.ino's single translation unit: included only by lcd43b.ino,
// inside its anonymous namespace, in the order listed there. Earlier parts
// provide the declarations later parts use. Do not include it anywhere else.
#pragma once
void tickUpdateBoot() {
    if (!updateBoot.pending) return;
    if (updateManifestVerified.load() && WiFi.status() == WL_CONNECTED
        && state.drawn && panel.waitForPresent()) {
        const auto before = panel.health();
#if defined(VENTANAL_LCD_LIT_CONFIRM) && defined(CONFIG_SPIRAM_XIP_FROM_PSRAM)
        const bool lit = backlightState;
#else
        ScanoutSavePause pause;
        const bool lit = false;
#endif
        const bool confirmed = lcd::confirmUpdate(prefs, updateBoot);
        if (confirmed) {
            settingsRefreshPending = true;
            panel.waitForVsync();
            const auto after = panel.health();
            lcd::log("ota confirm-scan lit=%d restart=%u short=%u peak_us=%u max_gap_us=%u\n",
                     lit, unsigned(after.lateRestarts - before.lateRestarts),
                     unsigned(after.shortFrames - before.shortFrames),
                     unsigned(after.maxFillUs), unsigned(after.maxVsyncGapUs));
        }
    }
}

bool showUpdating() {
    const int local = drawLocalScene([](lcd::SettingsSurface band) {
        lcd::drawSettingsChrome(band, false);
        band.text(StatusFont, "Updating...", 150, 210, lcd::rgb565(255,170,0));
    });
    if (local < 0) return false;
    clockSlot = -1;
    if (!presentScreen(nullptr, true, nullptr, local) || !panel.waitForPresent() || !panel.waitForVsync()
        || !setBacklight(true)) {
        backlightRefresh();
        return false;
    }
    lcd::log("ota update-screen=presented\n");
    return true;
}

void runQueuedUpdate() {
    if (refreshBusy) return;
    if (installQueued != lcd::UpdateSource::None) {
        const auto source = installQueued;
        installQueued = lcd::UpdateSource::None;
        const auto& current = source == lcd::UpdateSource::Host ? hostOffer : githubOffer;
        if (!current.version[0] || !lcd::sameOffer(current, queuedOffer)) {
            updateNotice = "Offer changed. Select the current update.";
        } else if (!showUpdating()) {
            updateNotice = "Update screen unavailable";
        } else {
            delay(1000);  // Leave the notice visible before blanking for the transfer.
            if (setBacklight(false)) {
                if (settingsDirty && lcd::saveSettings(settingsPrefs, settings)) settingsDirty = false;
                refreshWatchdog.subscribe();
                const auto failure = lcd::installUpdate(prefs, updateBoot, current, source, refreshWatchdog);
                refreshWatchdog.unsubscribe();
                if (failure == OtaFailure::None) {
                    lcd::log("ota reboot=%s\n", current.version);
                    esp_restart();
                }
                if (failure == OtaFailure::Download) wifiRetry.finished(millis(), false);
                char report[otaReportCapacity];
                formatOtaReport(report, updateBoot.journal.record);
                lcd::log("ota=%s\n", report);
                updateNotice = failure == OtaFailure::Attempts
                    ? "Attempt limit reached for this update" : "Installation failed";
            } else {
                updateNotice = "Could not blank the display";
            }
            settingsRefreshPending = true;  // Deliver the failure at the next check-in.
            powerSession.activity(millis());
        }
        if (settingsPage.active) showSettings();
        else {
            show(screenIndex);
        }
        panel.waitForPresent();
        backlightRefresh();
    } else if (checkQueued) {
        checkQueued = false;
        if (!startRefresh(false, true)) {
            updateNotice = "Update check could not start";
            if (settingsPage.active) showSettings();
        }
    }
}
