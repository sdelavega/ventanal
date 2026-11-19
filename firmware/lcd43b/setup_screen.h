// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
//
// The Wi-Fi setup page: the hotspot's name and password, the portal address,
// a QR code phones scan to join, the portal's status and (when setup was
// opened from Settings) a Cancel button. Same LCARS chrome as Settings.
#pragma once

#include "settings_page.h"
#include <algorithm>
#include <cstring>

namespace lcd {

inline constexpr ReadoutRect SETUP_TEXT{150, 90, 340, 280};
inline constexpr ReadoutRect SETUP_QR_AREA{500, 96, 276, 276};
inline constexpr ReadoutRect SETUP_CANCEL{150, 384, 200, 56};
inline constexpr int QR_MAX_MODULES = 41;  // QR version 6; the hotspot payload needs version 3-4

struct QrBits {
    int size = 0;
    uint8_t bits[(QR_MAX_MODULES * QR_MAX_MODULES + 7) / 8]{};
    void set(int x, int y, bool dark) {
        const int i = y * QR_MAX_MODULES + x;
        if (dark) bits[i / 8] |= uint8_t(1u << (i % 8));
        else bits[i / 8] &= uint8_t(~(1u << (i % 8)));
    }
    bool get(int x, int y) const {
        const int i = y * QR_MAX_MODULES + x;
        return bits[i / 8] >> (i % 8) & 1u;
    }
};

struct SetupQrLayout {
    int modulePx;
    ReadoutRect box;  // includes the four-module quiet zone on every side
};

inline SetupQrLayout setupQrLayout(int modules) {
    const int px = std::min(8, SETUP_QR_AREA.w / (modules + 8));
    const int side = (modules + 8) * px;
    return {px, {SETUP_QR_AREA.x + (SETUP_QR_AREA.w - side) / 2,
                 SETUP_QR_AREA.y + (SETUP_QR_AREA.h - side) / 2, side, side}};
}

struct SetupScreen {
    const char* apName;
    const char* apPassword;
    const char* status;   // the portal's latest message
    const char* note;     // optional second line, e.g. why setup opened
    bool cancel;
    const QrBits* qr;     // null or size 0: no code drawn
};

// Draws text cut to maxWidth, ending in "..." when it had to be cut.
inline void drawFitted(SettingsSurface px, const AAFont& font, const char* text, int x, int baseline,
                       int maxWidth, uint16_t colour) {
    char line[128];
    strncpy(line, text, sizeof line - 1);
    line[sizeof line - 1] = '\0';
    size_t n = strlen(line);
    if (textWidth(font, line) > maxWidth) {
        while (n && (textWidth(font, line) + textWidth(font, "...") > maxWidth)) line[--n] = '\0';
        strcat(line, "...");
    }
    px.text(font, line, x, baseline, colour);
}

inline void drawSetupPage(SettingsSurface px, const SetupScreen& s) {
    const auto gold = rgb565(255, 170, 0), light = rgb565(153, 150, 186);
    const auto sky = rgb565(170, 170, 255), white = rgb565(255, 255, 255);
    drawSettingsChrome(px, false);
    px.text(StatusFont, "WI-FI SETUP", 150, 63, gold);
    px.text(ArtDate, "ON YOUR PHONE, JOIN THIS WI-FI NETWORK", 150, 112, light);
    px.text(StatusFont, s.apName, 150, 148, gold);
    px.text(ArtDate, "PASSWORD", 150, 186, light);
    px.text(StatusFont, s.apPassword, 150, 222, white);
    px.text(ArtDate, "OR SCAN THE CODE. THEN OPEN THIS PAGE:", 150, 262, light);
    px.text(ClockTime, "http://192.168.4.1", 150, 292, gold);
    if (s.status && *s.status) drawFitted(px, ArtDate, s.status, 150, 330, SETUP_TEXT.w, sky);
    if (s.note && *s.note) drawFitted(px, ArtDate, s.note, 150, 354, SETUP_TEXT.w, light);
    if (s.cancel) updateButton(px, SETUP_CANCEL, "Cancel");
    if (!s.qr || s.qr->size <= 0 || s.qr->size > QR_MAX_MODULES) return;
    const SetupQrLayout q = setupQrLayout(s.qr->size);
    settingsBox(px, q.box, white);
    const int origin = 4 * q.modulePx;
    for (int y = 0; y < s.qr->size; ++y) {
        const int top = q.box.y + origin + y * q.modulePx;
        if (top + q.modulePx <= px.first || top >= px.first + px.rows) continue;
        for (int x = 0; x < s.qr->size; ++x)
            if (s.qr->get(x, y))
                settingsBox(px, {q.box.x + origin + x * q.modulePx, top, q.modulePx, q.modulePx}, 0);
    }
}

}  // namespace lcd
