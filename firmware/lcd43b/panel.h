// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
#pragma once

#include <Arduino.h>
#include <atomic>
#include "scanout.h"
#include "scanout_diag.h"

extern "C" {
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_rgb.h"
}

class Panel {
public:
    static constexpr int WIDTH = 800;
    static constexpr int HEIGHT = 480;
    static constexpr size_t Bytes = size_t(WIDTH) * HEIGHT * sizeof(uint16_t);
    static constexpr int BounceRows = 20;
    static constexpr unsigned Pclk = 13000000;
    static constexpr unsigned VSyncPulse = 4;
    static constexpr unsigned VBackPorch = 8;
    // Leave the last EOF refill more time to finish before the core's
    // automatic VSYNC restart. 12 lines is the ST7262's maximum front porch.
    static constexpr unsigned VFrontPorch = 12;
    static constexpr unsigned LineClocks = WIDTH + 4 + 8 + 8;
    static constexpr unsigned DrainUs = BounceRows * LineClocks * 1000000ull / Pclk;
    static constexpr unsigned VBlankUs =
        (VSyncPulse + VBackPorch + VFrontPorch) * LineClocks * 1000000ull / Pclk;
    static_assert(HEIGHT % BounceRows == 0);

    bool begin(bool (*setLcdReset)(bool), esp_lcd_rgb_panel_vsync_cb_t observer = nullptr);
    bool stop();
    bool waitForVsync() const;
    bool waitForPresent() const;
    bool references(const uint16_t* pixels) const;

    // The loop is the only producer. Prepare returns a slot whose scene and
    // patch storage are no longer read by either RGB interrupt.
    int prepare() const { return panel_ && waitForPresent() ? 1 - submitted_ : -1; }
    void present(int slot, lcd::ScanFrame scene, uint8_t brightness);
    esp_lcd_panel_handle_t handle() const { return panel_; }

#ifdef VENTANAL_LCD_DIAG_PANEL
    void reportBounce(uint32_t now) { diag_.report(now, DrainUs, VBlankUs); }
#endif

private:
    static bool vsync(esp_lcd_panel_handle_t, const esp_lcd_rgb_panel_event_data_t*, void*);
    static bool fillBounce(esp_lcd_panel_handle_t, void*, int, int, void*);

    esp_lcd_panel_handle_t panel_ = nullptr;
    esp_lcd_rgb_panel_vsync_cb_t observer_ = nullptr;
    lcd::ScanFrame scenes_[2]{};
    std::atomic<unsigned> requested_{0}, active_{0};
    std::atomic<uint32_t> vsyncs_{0};
    int submitted_ = 0;
    std::atomic<unsigned> armed_{0}; // VSYNC writes it; the next row-zero refill consumes it.
#ifdef VENTANAL_LCD_DIAG_PANEL
    void sampleDma();
    int dmaChannel_ = -1;
    lcd::ScanoutDiag diag_;
    uint32_t syncAt_ = 0;
#endif
};
