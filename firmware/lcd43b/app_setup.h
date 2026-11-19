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
    if (lcd::validInstallation(lcd::installation) && lcd::standalone(lcd::installation))
        return "Standalone mode needs a later update. Use a host for now.";
    return setupReason == lcd::SetupReason::FirstBoot ? "This board is not set up yet." : "";
}

bool showSetup() {
    const lcd::SetupScreen screen{setupApName, setupApPassword, setupPortal.status(), setupNote(),
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
    return setupPortal.begin(setupApName, setupApPassword, hooks);
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
    return true;
}

void leaveSetup() {
    setupPortal.end();
    WiFi.mode(WIFI_OFF);  // the next refresh joins as a station again
    setupActive = false;
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
        if (setupShownStatus != setupPortal.status()) showSetup();
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
