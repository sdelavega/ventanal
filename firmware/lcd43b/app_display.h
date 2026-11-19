// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
//
// Scene composition, presenting screens and backlight policy.
//
// Part of lcd43b.ino's single translation unit: included only by lcd43b.ino,
// inside its anonymous namespace, in the order listed there. Earlier parts
// provide the declarations later parts use. Do not include it anywhere else.
#pragma once
// Settings owns the panel while open. Every other redraw routes here, and
// minute updates use CLOCK_GAP even when the underlying selection is artwork.
void closeSettings(const char* reason);
constexpr bool patchFitsPanel(lcd::ScanRect r) {
    return r.x >= 0 && r.y >= 0 && r.w > 0 && r.h > 0 &&
           r.w <= Panel::WIDTH - r.x && r.h <= Panel::HEIGHT - r.y;
}

void copyPatch(uint16_t* out, const uint16_t* source, lcd::ScanRect r) {
    for (int y = 0; y < r.h; ++y)
        memcpy(out + y * r.w, source + (r.y + y) * lcd::WIDTH + r.x, size_t(r.w) * 2);
}

template<class Paint>
int drawLocalScene(Paint paint, lcd::ReadoutRect damage = {}) {
    if (!canvas || !beginPanel() || !panel.waitForPresent()) return -1;
    const int next = displayedLocal == 0 ? 1 : 0;
    auto* page = localPages[next];
    if (!page) return -1;
    for (int y = 0; y < lcd::HEIGHT; y += CanvasRows) {
        static_assert(lcd::HEIGHT % CanvasRows == 0);
        const bool unchanged = damage.h && displayedLocal >= 0 &&
            (y >= damage.y + damage.h || y + CanvasRows <= damage.y);
        bool stored;
        if (unchanged) {
            stored = page->copyBand(*localPages[displayedLocal], y, CanvasRows);
        } else {
            paint(lcd::SettingsSurface{canvas, y, CanvasRows});
            lcd::dimScanPixels(canvas, lcd::WIDTH * CanvasRows, settings.brightness / 25);
            stored = page->encodeBand(canvas, y, CanvasRows);
        }
        if (!stored) {
            lcd::log("settings error=packed-page-limit\n");
            return -1;
        }
    }
    return next;
}

// Publish the resident image and small, already rendered patches together.
bool presentScreen(const uint16_t* source, bool localSettings = false,
                   bool* clockValid = nullptr, int local = -1) {
    if (!beginPanel()) return false;
    const int sceneIndex = panel.prepare();
    if (sceneIndex < 0) return false;
    lcd::ScanFrame scene{};
    scene.pixels = source;
    if (local >= 0) scene.local = localPages[local];
    if (source && !localSettings) {
        if (auto* status = statusPixels[sceneIndex]) {
            if (state.offline) {
                constexpr lcd::ScanRect r{0, 0, StatusWidth, OfflineHeight};
                static_assert(patchFitsPanel(r));
                copyPatch(status->offline, source, r);
                lcd::drawOfflineMark(status->offline, r.w, r.h);
                scene.patches[0] = {r, status->offline};
            }
            if (const char* text = lcd::refreshLabelText(refreshLabel.label())) {
                constexpr lcd::ScanRect r{0, lcd::HEIGHT - 46, StatusWidth, RefreshHeight};
                static_assert(patchFitsPanel(r));
                copyPatch(status->refresh, source, r);
                const bool red = refreshLabel.label() == lcd::RefreshLabel::Offline
                              || refreshLabel.label() == lcd::RefreshLabel::Failed;
                lcd::drawRefreshBadge(status->refresh, r.w, r.h, text,
                                      red ? lcd::RGB565_RED : lcd::RGB565_WHITE, 32);
                scene.patches[1] = {r, status->refresh};
            }
        }
    }
    if (clockSlot >= 0 && clockPixels[sceneIndex]) {
        auto* pixels = clockPixels[sceneIndex];
        static_assert(patchFitsPanel(lcd::CLOCK_GAP) && patchFitsPanel(lcd::ART_CORNER));
        const auto r = lcd::readoutRect(clockSlot);
        if (!localSettings && !lcd::isLcarsSlot(clockSlot)) copyPatch(pixels, source, r);
        const bool valid = renderClock(pixels);
        if (clockValid) *clockValid = valid;
        if (local >= 0) lcd::dimScanPixels(pixels, r.w * r.h, settings.brightness / 25);
        scene.patches[2] = {r, pixels};
    }
    // Local bands and their clock patch are already dimmed. Keep both
    // raster decompression and pixel-by-pixel scaling out of one ISR pass.
    panel.present(sceneIndex, scene, local >= 0 ? 100 : settings.brightness);
    displayedLocal = local;
    return true;
}

bool showSettings(lcd::ReadoutRect damage) {
    char footer[192];
    snprintf(footer, sizeof footer, "VENTANAL · %s · WI-FI %s · v%s",
             lcd::installation.profile, WiFi.status() == WL_CONNECTED ? "OK" : "OFFLINE",
             lcd::FIRMWARE_VERSION);
    const bool busy = checkQueued || refreshCheckingUpdates || installQueued != lcd::UpdateSource::None;
    const int local = drawLocalScene([&](lcd::SettingsSurface pixels) {
        if (updatesPage) {
            const bool waiting = installQueued != lcd::UpdateSource::None;
            lcd::drawUpdatePage(pixels, hostOffer.version, githubOffer.version,
                                waiting ? "Waiting for refresh..." : updateNotice, busy, waiting);
        } else {
            lcd::drawSettingsPage(pixels, settings, solarPublished(), settingsBatteryButton(), footer);
            if (!updateBoot.pending) {
                const char* version = hostOffer.version[0] ? hostOffer.version : githubOffer.version;
                char label[48] = "Firmware updates";
                if (version[0]) lcd::updateTitle(label, version);
                lcd::updateButton(pixels, lcd::UPDATE_MENU, label);
            }
        }
    }, damage);
    clockSlot = clockBuffersReady() ? 1 : -1;
    const bool ok = local >= 0 && presentScreen(nullptr, true, nullptr, local);
    lcd::log("settings draw=%d brightness=%u solarListed=%d batteryButton=%d\n",
             ok, settings.brightness, solarPublished(), settingsBatteryButton());
    return ok;
}

bool showSettings() { return showSettings({}); }

const uint8_t* screenFrame(int slot) {
    if (slot == 0 && owner.overrideArt.id[0]) return overrideFrame;
    return state.cachedIds[slot][0] ? residentFrames[slot] : nullptr;
}

bool show(int index) {
    if (!powerSession.screenOn || powerSession.chargeShowing) return false;
    if (setupActive) return showSetup();
    if (settingsPage.active) return showSettings();
    const int slot = screenSlot(index);
    if (slot < 0) {
        lcd::log("blit error=screen-index index=%d\n", index);
        return false;
    }
    const uint32_t started = micros();
    const uint8_t* frame = screenFrame(slot);
    const uint16_t* pixels = nullptr;
    clockSlot = frame && clockBuffersReady() ? slot : -1;
    if (frame) pixels = framePixels(frame);
    int local = -1;
    if (!frame) {
        local = drawLocalScene([&](lcd::SettingsSurface band) {
            lcd::drawWaiting(band.pixels, lcd::WIDTH, band.rows,
                             installationReady ? lcd::installation.host : "Provision paired HTTPS over USB",
                             installationReady, band.first);
            if (const char* text = lcd::refreshLabelText(refreshLabel.label())) {
                const bool red = refreshLabel.label() == lcd::RefreshLabel::Offline
                              || refreshLabel.label() == lcd::RefreshLabel::Failed;
                lcd::drawRefreshBadge(band.pixels, lcd::WIDTH, band.rows, text,
                    red ? lcd::RGB565_RED : lcd::RGB565_WHITE, lcd::HEIGHT - 14 - band.first);
            }
        });
        if (local < 0) return false;
    }
    const bool ok = presentScreen(pixels, false, nullptr, local) && frame;
    state.drawn = ok;
    if (ok) lcd::log("photo-ready slot=%s clock=board region-only-minute-redraw battery=none\n", lcd::SLOT_NAMES[slot]);
    lcd::log("blit screen=%s id=%.16s cache=%s offline=%d brightness=%u ms=%lu\n",
             lcd::SLOT_NAMES[slot], slot == 0 && owner.overrideArt.id[0] ? owner.overrideArt.id : state.cachedIds[slot],
             ok ? "frame" : "waiting", state.offline, settings.brightness,
             static_cast<unsigned long>((micros() - started) / 1000));
    return ok;
}

void tickClock() {
    if (!powerSession.screenOn || powerSession.chargeShowing || clockSlot < 0 ||
        (!settingsPage.active && !state.drawn) || time(nullptr) / 60 == drawnMinute) return;
    const auto* frame = settingsPage.active ? nullptr : screenFrame(clockSlot);
    const auto* source = framePixels(frame);
    if (!source && displayedLocal < 0) return;
    bool valid = false;
    const bool ok = presentScreen(source, settingsPage.active, &valid, displayedLocal);
    const auto r = lcd::readoutRect(clockSlot);
    lcd::log("clock minute valid=%d slot=%s region=%d,%d,%d,%d blit=%d brightness=%u battery=none\n",
             valid, settingsPage.active ? "settings" : lcd::SLOT_NAMES[clockSlot], r.x, r.y, r.w, r.h,
             ok, settings.brightness);
}

bool setBacklight(bool on) {
    if (backlightKnown && on == backlightState) return true;
    backlightKnown = expander.setBacklight(on);
    if (!backlightKnown) {
        lcd::log("backlight error=expander requested=%d\n", on);
        return false;
    }
#ifdef VENTANAL_LCD_DIAG_PANEL
    if (on) lcd::scanLit.store(true, std::memory_order_relaxed);
#endif
    backlightState = on;
#ifdef VENTANAL_LCD_DIAG_PANEL
    if (!on) lcd::scanLit.store(false, std::memory_order_relaxed);
#endif
    lcd::log("backlight on=%d\n", on);
    return true;
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
lcd::DisplayInputs displayInputs() {
    lcd::DisplayInputs in;
    in.panelRunning = panel.handle() != nullptr;
    in.screenOn = powerSession.screenOn;
    in.chargeShowing = powerSession.chargeShowing;
    in.installQueued = installQueued != lcd::UpdateSource::None || updateScreenShowing;
    in.now = millis();
    in.touchWakeUntil = touchUntilMs;
    if (in.screenOn && !in.chargeShowing) {
        tm local{};
        const time_t now = time(nullptr);
        localtime_r(&now, &local);
        in.quiet = lcd::quietApplies(local.tm_year + 1900,
                                     local.tm_hour * 60 + local.tm_min,
                                     state.manifest);
    }
    return in;
}

const char* lastDisplayReason = nullptr;

// Apply the display owner's decision. The owner is the only authority on
// whether the screen may be lit; this is the only place that asks it.
void backlightRefresh() {
    const auto in = displayInputs();
    quietBlanking = displayOwner.quietBlanking(in);
    const auto decision = displayOwner.decide(in, true);
    if (decision.reason != lastDisplayReason) {
        lastDisplayReason = decision.reason;
        lcd::log("display reason=%s\n", decision.reason);
    }
    setBacklight(decision.lit);
}
