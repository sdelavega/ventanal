// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
#include "../lcd43b/setup_record.h"
#include "../lcd43b/setup_screen.h"
#include "../lcd43b/local_scene.h"
#include <memory>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace lcd;

static bool overlaps(ReadoutRect a, ReadoutRect b) {
    return a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h;
}

static void setup_control_hit() {
    const ReadoutRect r = settingsDamage(SettingsHit::SetupWifi);
    assert(r.w && r.h >= 52);
    assert(hitSettings(r.x + r.w / 2, r.y + r.h / 2, false, false) == SettingsHit::SetupWifi);
    assert(hitSettings(r.x + r.w / 2, r.y + r.h / 2, true, true) == SettingsHit::SetupWifi);
    for (const auto& c : SETTINGS_CONTROLS)
        if (c.hit != SettingsHit::SetupWifi) assert(!overlaps(c.rect, r));
    assert(!overlaps(r, UPDATE_MENU));
    DeviceSettings s = defaultSettings(false);
    assert(!applySettingsHit(s, SettingsHit::SetupWifi, true, 4200));  // not a device setting
}

static void setup_screen_layout() {
    for (int modules : {25, 29, 33, 41}) {
        const SetupQrLayout q = setupQrLayout(modules);
        assert(q.modulePx >= 4);
        assert(q.box.w == (modules + 8) * q.modulePx && q.box.h == q.box.w);
        assert(q.box.x >= SETUP_QR_AREA.x && q.box.x + q.box.w <= SETUP_QR_AREA.x + SETUP_QR_AREA.w);
        assert(q.box.y >= SETUP_QR_AREA.y && q.box.y + q.box.h <= SETUP_QR_AREA.y + SETUP_QR_AREA.h);
        assert(!overlaps(q.box, SETUP_CANCEL) && !overlaps(q.box, SETUP_TEXT));
    }
    assert(setupQrLayout(33).modulePx >= 6);  // the usual hotspot payload
    assert(SETUP_TEXT.x >= 150 && SETUP_TEXT.x + SETUP_TEXT.w <= SETUP_QR_AREA.x);
    assert(!overlaps(SETUP_CANCEL, SETUP_TEXT));
    assert(SETUP_CANCEL.x + SETUP_CANCEL.w <= WIDTH && SETUP_CANCEL.y + SETUP_CANCEL.h <= HEIGHT);
}

static void setup_screen_draws_qr_modules() {
    QrBits qr{};
    qr.size = 33;
    for (int y = 0; y < qr.size; ++y)
        for (int x = 0; x < qr.size; ++x) qr.set(x, y, (x + y) % 2 == 0);
    std::vector<uint16_t> px(size_t(WIDTH) * HEIGHT, 0x1234);
    SetupScreen screen{"Ventanal-3C0F", "abcDEF2345", "Checking", false, "", true, &qr};
    drawSetupPage(SettingsSurface(px.data()), screen);
    const SetupQrLayout q = setupQrLayout(qr.size);
    auto at = [&](int x, int y) { return px[size_t(y) * WIDTH + x]; };
    const int origin = 4 * q.modulePx;  // quiet zone
    assert(at(q.box.x + 1, q.box.y + 1) == 0xffff);                         // quiet zone is white
    assert(at(q.box.x + origin + 1, q.box.y + origin + 1) == 0);            // module (0,0) is dark
    assert(at(q.box.x + origin + q.modulePx + 1, q.box.y + origin + 1) == 0xffff);  // (1,0) light
    // Banded drawing gives the same pixels as a whole-frame draw.
    std::vector<uint16_t> band(size_t(WIDTH) * 40);
    for (int first = 0; first < HEIGHT; first += 40) {
        drawSetupPage(SettingsSurface(band.data(), first, 40), screen);
        assert(!memcmp(band.data(), px.data() + size_t(first) * WIDTH, band.size() * sizeof band[0]));
    }
}

static void long_status_stays_left_of_the_code() {
    std::vector<uint16_t> px(size_t(WIDTH) * HEIGHT, 0x1234);
    const std::string status = "Couldn't join \"" + std::string(32, 'W') + "\". Check the network and its password.";
    SetupScreen screen{"Ventanal-3C0F", "abcDEF2345", status.c_str(), true, status.c_str(), false, nullptr};
    drawSetupPage(SettingsSurface(px.data()), screen);
    for (int y = 300; y < 370; ++y)
        for (int x = SETUP_TEXT.x + SETUP_TEXT.w; x < 790; ++x) assert(px[size_t(y) * WIDTH + x] == 0);
    bool inked = false;
    for (int x = 150; x < 400; ++x) inked = inked || px[size_t(322) * WIDTH + x] != 0;
    assert(inked);
}

static void setup_page_fits_local_scene() {
    QrBits qr{};
    qr.size = QR_MAX_MODULES;
    uint32_t seed = 7;
    for (int y = 0; y < qr.size; ++y)
        for (int x = 0; x < qr.size; ++x) {
            seed = seed * 1664525u + 1013904223u;
            qr.set(x, y, seed >> 31);
        }
    const std::string status = "Couldn't join \"" + std::string(32, 'W') + "\". Check the network.";
    SetupScreen screen{"Ventanal-WWWW", "WWWWWWWWWW", status.c_str(), true, status.c_str(), true, &qr};
    std::vector<uint16_t> px(size_t(WIDTH) * HEIGHT);
    drawSetupPage(SettingsSurface(px.data()), screen);
    auto page = std::make_unique<LocalPage>();
    assert(page->encode(px.data()));
    printf("setup page packs into %u of %u words\n", unsigned(page->rows[HEIGHT]), unsigned(48 * 1024));
    assert(page->rows[HEIGHT] < 40 * 1024);  // headroom under the 48K-word page
}

static portal::PortalFields fields() {
    portal::PortalFields f{};
    strcpy(f.ssid, "synthetic-network");
    strcpy(f.password, "synthetic-password");
    strcpy(f.zip, "90210");
    f.units = 1;
    f.clock24h = 0;
    f.quietStart = 22 * 60;
    f.quietEnd = 7 * 60;
    strcpy(f.callSign, "K1ABC");
    return f;
}

static portal::LocationResult located() {
    portal::LocationResult l;
    l.ok = true;
    l.why = portal::LocationResult::Ok;
    l.latitude = 34.0901f;
    l.longitude = -118.4065f;
    strcpy(l.place, "Beverly Hills, CA");
    strcpy(l.posixTz, "PST8PDT,M3.2.0,M11.1.0");
    return l;
}

constexpr time_t PortalNow = 1704067200;

static void first_setup_builds_a_standalone_record() {
    const Installation none{};
    const Installation r = installationFromPortal(none, fields(), located(), PortalNow);
    assert(validInstallation(r) && standalone(r));
    assert(!strcmp(r.ssid, "synthetic-network") && !strcmp(r.zip, "90210") && r.units == 1 && !r.clock24h);
    assert(r.quietStart == 22 * 60 && r.quietEnd == 7 * 60 && !strcmp(r.callSign, "K1ABC"));
    assert(!strcmp(r.timezone, "PST8PDT,M3.2.0,M11.1.0") && !strcmp(r.place, "Beverly Hills, CA"));
    assert(std::fabs(r.latitude - 34.0901f) < 1e-4f && !strcmp(r.profile, "lcd43b"));
    assert(r.callState == uint8_t(CallState::Unverified) && r.callCheckedAt == PortalNow);
}

static Installation pairedRecord() {
    Installation r{};
    strcpy(r.ssid, "old-network");
    strcpy(r.password, "old-password");
    strcpy(r.host, "https://frame.test/");
    strcpy(r.timezone, "EST5EDT,M3.2.0,M11.1.0");
    strcpy(r.profile, "desk-display");
    strcpy(r.pairing, "desk");
    strcpy(r.certificate, "-----BEGIN CERTIFICATE-----x");
    memset(r.deviceKey, 'a', 64);
    r.forcedRevision = 3;
    strcpy(r.callSign, "K1ABC");
    r.callState = uint8_t(CallState::Verified);
    r.callCheckedAt = 1234;
    return r;
}

static void setup_keeps_an_existing_host_pairing() {
    const Installation current = pairedRecord();
    const Installation r = installationFromPortal(current, fields(), located(), PortalNow);
    assert(validInstallation(r) && !standalone(r));
    assert(!strcmp(r.host, "https://frame.test/") && !strcmp(r.pairing, "desk") && r.deviceKey[0] == 'a');
    assert(!strcmp(r.profile, "desk-display") && r.forcedRevision == 3);  // a private seed stays write-once
    assert(!strcmp(r.ssid, "synthetic-network"));
    // Saving also checks an unchanged call sign.
    assert(r.callState == uint8_t(CallState::Unverified) && r.callCheckedAt == PortalNow);
    // No ZIP lookup (host join without a ZIP) keeps the stored zone.
    portal::PortalFields noZip = fields();
    noZip.zip[0] = '\0';
    portal::LocationResult skipped;
    skipped.why = portal::LocationResult::Skipped;
    const Installation kept = installationFromPortal(current, noZip, skipped, PortalNow);
    assert(!strcmp(kept.timezone, "EST5EDT,M3.2.0,M11.1.0") && std::isnan(kept.latitude));
}

static void setup_applies_the_call_sign_trust_fallback() {
    const Installation current = pairedRecord();
    portal::PortalFields other = fields();
    strcpy(other.callSign, "W1AW");
    const Installation changed = installationFromPortal(current, other, located(), 1000);
    assert(changed.callState == uint8_t(CallState::Unverified) && !changed.callCheckedAt);
    other.callSign[0] = '\0';
    const Installation cleared = installationFromPortal(current, other, located(), PortalNow);
    assert(!cleared.callSign[0] && cleared.callState == uint8_t(CallState::None) && !cleared.callCheckedAt);
}

static void setup_uses_portal_host_fields() {
    portal::PortalFields f = fields();
    strcpy(f.host, "https://other.test");
    strcpy(f.pairing, "lounge");
    strcpy(f.certificate, "-----BEGIN CERTIFICATE-----y");
    memset(f.deviceKey, 'b', 64);
    const Installation r = installationFromPortal(pairedRecord(), f, located(), PortalNow);
    assert(!strcmp(r.host, "https://other.test") && !strcmp(r.pairing, "lounge") && r.deviceKey[0] == 'b');
}

static void leave_host_clears_credentials_keeps_zip() {
    portal::PortalFields f = fields();
    f.leaveHost = 1;
    const Installation r = installationFromPortal(pairedRecord(), f, located(), PortalNow);
    assert(validInstallation(r) && standalone(r));
    assert(!r.host[0] && !r.pairing[0] && !r.certificate[0] && !r.deviceKey[0]);
    assert(!strcmp(r.zip, "90210") && !strcmp(r.timezone, "PST8PDT,M3.2.0,M11.1.0"));
    assert(!strcmp(r.profile, "desk-display") && r.forcedRevision == 3);
}

static void portal_starts_from_the_record() {
    const Installation current = pairedRecord();
    const portal::PortalFields f = portalFieldsFrom(current);
    assert(!strcmp(f.ssid, "old-network") && !strcmp(f.password, "old-password"));
    assert(!strcmp(f.host, "https://frame.test/") && !strcmp(f.pairing, "desk") && f.deviceKey[0] == 'a');
    assert(!strcmp(f.callSign, "K1ABC") && f.quietStart == current.quietStart && f.clock24h == current.clock24h);
    // Saving the untouched form gives back the same pairing.
    const Installation same = installationFromPortal(current, f, portal::LocationResult{}, PortalNow);
    assert(!strcmp(same.host, current.host) && !strcmp(same.certificate, current.certificate));
}

static void setup_screen_has_the_iphone_hint() {
    std::vector<uint16_t> px(size_t(WIDTH) * HEIGHT, 0x1234);
    SetupScreen screen{"Ventanal-3C0F", "abcDEF2345", "", false, "", true, nullptr};
    drawSetupPage(SettingsSurface(px.data()), screen);
    bool inked = false;
    for (int y = SETUP_HINT.y; y < SETUP_HINT.y + SETUP_HINT.h; ++y)
        for (int x = SETUP_HINT.x; x < SETUP_HINT.x + SETUP_HINT.w; ++x) inked = inked || px[size_t(y) * WIDTH + x];
    assert(inked);
    assert(!overlaps(SETUP_HINT, SETUP_CANCEL) && !overlaps(SETUP_HINT, setupQrLayout(QR_MAX_MODULES).box));
    assert(SETUP_HINT.y >= SETUP_QR_AREA.y + SETUP_QR_AREA.h && SETUP_HINT.y + SETUP_HINT.h <= HEIGHT);
}

static void first_boot_without_record_enters_setup() {
    assert(bootSetup(false, false) == BootSetup::FirstBoot);   // nothing stored
    assert(bootSetup(true, false) == BootSetup::FirstBoot);    // stored but not usable here yet
    assert(bootSetup(true, true) == BootSetup::None);
}

static void cancel_returns_when_record_usable() {
    using portal::PortalOutcome;
    assert(afterSetup(PortalOutcome::Saved, SetupReason::UserRequested) == SetupNext::Restart);
    assert(afterSetup(PortalOutcome::Saved, SetupReason::FirstBoot) == SetupNext::Restart);
    assert(afterSetup(PortalOutcome::Cancelled, SetupReason::UserRequested) == SetupNext::Return);
    assert(afterSetup(PortalOutcome::TimedOut, SetupReason::UserRequested) == SetupNext::Return);
    assert(afterSetup(PortalOutcome::TimedOut, SetupReason::FirstBoot) == SetupNext::Reopen);
    assert(afterSetup(PortalOutcome::Running, SetupReason::FirstBoot) == SetupNext::Stay);
    assert(setupCancelVisible(SetupReason::UserRequested) && !setupCancelVisible(SetupReason::FirstBoot));
}

int main() {
    setup_control_hit();
    setup_screen_layout();
    setup_screen_draws_qr_modules();
    long_status_stays_left_of_the_code();
    setup_page_fits_local_scene();
    first_setup_builds_a_standalone_record();
    setup_keeps_an_existing_host_pairing();
    setup_applies_the_call_sign_trust_fallback();
    setup_uses_portal_host_fields();
    leave_host_clears_credentials_keeps_zip();
    portal_starts_from_the_record();
    setup_screen_has_the_iphone_hint();
    first_boot_without_record_enters_setup();
    cancel_returns_when_record_usable();
    puts("lcd43b setup: Settings control, setup screen and QR, record from the portal, boot and exit rules passed");
}
