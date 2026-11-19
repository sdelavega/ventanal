// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
//
// Wi-Fi setup: the shared portal (WPA2 hotspot + captive page) behind an
// LCARS page that shows the hotspot's name, password and a QR code.
//
// Part of lcd43b.ino's single translation unit: included only by lcd43b.ino,
// inside its anonymous namespace, in the order listed there.
#pragma once

portal::SetupPortal setupPortal;
lcd::SetupReason setupReason = lcd::SetupReason::FirstBoot;
char setupApName[16]{};
char setupApPassword[11]{};
lcd::QrBits setupQr;
std::string setupShownStatus;
// Diagnostics only: run setup with the panel stopped and the backlight off.
#ifdef VENTANAL_LCD_DIAG_SETUP_DARK
bool setupDark = true;
#else
bool setupDark = false;
#endif
bool setupDarkHeld = false;

void captureSetupQr(esp_qrcode_handle_t code) {
    const int size = esp_qrcode_get_size(code);
    setupQr = lcd::QrBits{};
    if (size <= 0 || size > lcd::QR_MAX_MODULES) return;
    setupQr.size = size;
    for (int y = 0; y < size; ++y)
        for (int x = 0; x < size; ++x) setupQr.set(x, y, esp_qrcode_get_module(code, x, y));
}

void buildSetupQr() {
    esp_qrcode_config_t config{};
    config.display_func = captureSetupQr;
    config.max_qrcode_version = 6;  // QR_MAX_MODULES
    config.qrcode_ecc_level = ESP_QRCODE_ECC_MED;
    setupQr = lcd::QrBits{};
    const std::string payload = portal::wifiQrPayload(setupApName, setupApPassword);
    if (esp_qrcode_generate(&config, payload.c_str()) != ESP_OK || !setupQr.size)
        lcd::log("setup error=qr\n");
}

// The portal's save hook: one write of the whole record, with scanout
// paused like every other lit NVS write. The board restarts afterwards.
bool saveSetupRecord(const portal::PortalFields& fields, const portal::LocationResult& where) {
    auto next = std::make_unique<lcd::Installation>();  // 4.8 KB: not on the loop stack
    lcd::installationFromPortal(lcd::installation, fields, where, *next);
    if (!lcd::validInstallation(*next)) {
        lcd::log("setup save=0 reason=invalid\n");
        return false;
    }
    bool saved = false;
    {
        ScanoutSavePause pause;
        saved = lcd::saveInstallation(*next);  // two-slot store: a failed write keeps the old record
    }
    lcd::log("setup save=%d mode=%s\n", saved, lcd::standalone(*next) ? "standalone" : "host");
    return saved;
}

const char* setupNote() {
    return setupReason == lcd::SetupReason::FirstBoot ? "This board is not set up yet." : "";
}

bool showSetup() {
    const lcd::SetupScreen screen{setupApName, setupApPassword, setupPortal.status(),
                                  setupPortal.statusIsFailure(), setupNote(),
                                  lcd::setupCancelVisible(setupReason), &setupQr};
    const int local = drawLocalScene([&](lcd::SettingsSurface pixels) { lcd::drawSetupPage(pixels, screen); });
    clockSlot = -1;
    const bool ok = local >= 0 && presentScreen(nullptr, true, nullptr, local);
    setupShownStatus = setupPortal.status();
    lcd::log("setup draw=%d\n", ok);
    return ok;
}

bool startSetupPortal() {
    portal::PortalHooks hooks;
    hooks.join = portal::joinStation;
    hooks.locate = portal::resolveLocation;
    hooks.save = saveSetupRecord;
    hooks.feed = [] {};
    hooks.note = [](const char* line) { lcd::log("%s\n", line); };
    static portal::PortalFields current;  // 4.9 KB: not on the loop stack
    current = lcd::validInstallation(lcd::installation) ? lcd::portalFieldsFrom(lcd::installation)
                                                        : portal::PortalFields{};
    return setupPortal.begin(setupApName, setupApPassword, hooks, lcd::installation.ssid, &current);
}

bool enterSetup(lcd::SetupReason why) {
    if (setupActive) return true;
    if (refreshBusy || installQueued != lcd::UpdateSource::None) {
        setupQueued = true;  // the network is in use: start when it is free
        lcd::log("setup queued refresh_busy=%d\n", refreshBusy);
        return false;
    }
    setupQueued = false;
    setupReason = why;
    uint8_t mac[6]{};
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    strlcpy(setupApName, portal::apName(mac).c_str(), sizeof setupApName);
#ifdef VENTANAL_LCD_DIAG_SETUP_VARIANTS
    {
        const char suffix[] = {'-', char('A' + portal::portalApVariant), '\0'};
        strlcat(setupApName, suffix, sizeof setupApName);
    }
#endif
    portal::apPassword(esp_random, setupApPassword);
    buildSetupQr();
    WiFi.disconnect(false);
    if (!startSetupPortal()) {
        lcd::log("setup error=portal-start\n");
        return false;
    }
    setupActive = true;
    settingsPage.active = false;
    updatesPage = false;
    touchPoller.controlsMode(true);
    powerSession.activity(millis());
    lcd::log("setup start reason=%s ap=%s\n", why == lcd::SetupReason::FirstBoot ? "first-boot" : "user",
             setupApName);
    showSetup();
    if (setupDark) {
        // Diagnostic: is scan-out (PSRAM traffic) what keeps phones from seeing
        // the hotspot? Hold the backlight off and stop the panel while setup runs.
        displayOwner.acquire(lcd::DarkReason::Diag, millis());
        backlightRefresh();
        setupDarkHeld = true;
        lcd::log("diag setup-dark panel_stopped=%d\n", stopPanel());
    }
    return true;
}

void leaveSetup() {
    setupPortal.end();
    WiFi.mode(WIFI_OFF);  // the next refresh joins as a station again
    setupActive = false;
    if (setupDarkHeld) {
        setupDarkHeld = false;
        displayOwner.release(lcd::DarkReason::Diag);
    }
    touchPoller.controlsMode(false);
    powerSession.activity(millis());
    lcd::log("setup leave\n");
    show(screenIndex);
}

void setupPoll(uint32_t now) {
    if (!setupActive) {
        if (setupQueued) enterSetup(lcd::SetupReason::UserRequested);
        return;
    }
    powerSession.activity(now);  // keep the password on screen while setup runs
    switch (lcd::afterSetup(setupPortal.poll(now), setupReason)) {
    case lcd::SetupNext::Stay:
        if (!setupDarkHeld && setupShownStatus != setupPortal.status()) showSetup();
        break;
    case lcd::SetupNext::Restart:
        lcd::log("setup saved restart=1\n");
        setupPortal.end();
        delay(100);
        ESP.restart();
        break;
    case lcd::SetupNext::Reopen:
        setupPortal.end();  // same name and password: the screen stays valid
        if (!startSetupPortal()) lcd::log("setup error=portal-restart\n");
        break;
    case lcd::SetupNext::Return:
        leaveSetup();
        break;
    }
}

void setupTap(int x, int y) {
    if (lcd::setupCancelVisible(setupReason) && lcd::inUpdateControl(lcd::SETUP_CANCEL, x, y)) {
        lcd::log("setup cancel x=%d y=%d\n", x, y);
        setupPortal.cancel();
    }
}

#ifdef VENTANAL_LCD_DIAG_SETUP_CYCLE
// Diagnostic: with no one at the board, open setup by itself six times,
// alternating lit and dark, 3 minutes each with 30 s of art in between, so
// a nearby Wi-Fi scanner can time when the hotspot first becomes visible.
struct DiagSetupCycle {
    int cycle = 0;
    bool running = false;
    uint32_t at = 15000;
} diagSetupCycle;

void diagSetupCycleTick(uint32_t now) {
    auto& d = diagSetupCycle;
    if (d.cycle >= 6) return;
    if (!d.running) {
        if (setupActive) {
            d.running = true;
            d.at = now + 180000;
            lcd::log("diag setup-cycle n=%d dark=%d start\n", d.cycle + 1, setupDark);
        } else if (int32_t(now - d.at) >= 0) {
            setupDark = d.cycle % 2;
            enterSetup(lcd::SetupReason::UserRequested);  // may queue behind a refresh
            d.at = now + 5000;
        }
    } else if (!setupActive) {
        lcd::log("diag setup-cycle n=%d end\n", d.cycle + 1);
        d.running = false;
        d.at = now + 30000;
        if (++d.cycle == 6) lcd::log("diag setup-cycle done\n");
    } else if (int32_t(now - d.at) >= 0) {
        setupPortal.cancel();
    }
}
#endif

#ifdef VENTANAL_LCD_DIAG_SETUP_VARIANTS
// Diagnostic: four automatic setup runs, 3 minutes each, one per hotspot
// variant (defaults, 20 MHz, US country, both), each under its own name
// (Ventanal-XXXX-A..D) so an Apple scanner's cache cannot mix them up.
struct DiagSetupVariants {
    int run = 0;
    bool running = false;
    uint32_t at = 15000;
} diagVariants;

void diagSetupVariantsTick(uint32_t now) {
    auto& d = diagVariants;
    static constexpr int variants[] = {4, 5};  // E: station off until a join; F: also 20 MHz
    constexpr int runs = sizeof variants / sizeof variants[0];
    if (d.run >= runs) return;
    if (!d.running) {
        if (setupActive) {
            d.running = true;
            d.at = now + 180000;
            lcd::log("diag setup-variant run=%d variant=%d ap=%s start\n", d.run + 1, portal::portalApVariant,
                     setupApName);
        } else if (int32_t(now - d.at) >= 0) {
            portal::portalApVariant = variants[d.run];
            enterSetup(lcd::SetupReason::UserRequested);
            d.at = now + 5000;
        }
    } else if (!setupActive) {
        lcd::log("diag setup-variant run=%d end\n", d.run + 1);
        d.running = false;
        d.at = now + 30000;
        if (++d.run == runs) lcd::log("diag setup-variant done\n");
    } else if (int32_t(now - d.at) >= 0) {
        setupPortal.cancel();
    }
}
#endif
