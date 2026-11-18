// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
#pragma once

// esp_lcd RGB panel driver for the 4.3B: an 800x480 ST7262-class parallel
// RGB565 panel. Pin map, timing (16 MHz PCLK, 4/8/8 porches) and the PSRAM
// frame buffer with DRAM bounce buffers follow Waveshare's own board
// support file for this display. The CH422G expander releases the panel's
// reset line (EXIO3) and enables the backlight (EXIO2) separately.

#include <Arduino.h>
#include <cstdint>
#include <cstddef>
#include "settings.h"

extern "C" {
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_rgb.h"
}

class Panel {
public:
    static constexpr int WIDTH = 800;
    static constexpr int HEIGHT = 480;

    // Pulse the panel reset through the CH422G EXIO3 line (active low), then
    // start the RGB panel. Returns false if the driver or PSRAM allocation fails.
    bool begin(bool (*setLcdReset)(bool)) {
        if(panel_) return true;
        setLcdReset(true);    // assert reset: EXIO3 low
        delay(10);
        setLcdReset(false);   // release
        delay(100);
        esp_lcd_rgb_panel_config_t config = {};
        config.clk_src = LCD_CLK_SRC_DEFAULT;
        config.timings.pclk_hz = 16 * 1000 * 1000;
        config.timings.h_res = WIDTH;
        config.timings.v_res = HEIGHT;
        config.timings.hsync_pulse_width = 4;
        config.timings.hsync_back_porch = 8;
        config.timings.hsync_front_porch = 8;
        config.timings.vsync_pulse_width = 4;
        config.timings.vsync_back_porch = 8;
        config.timings.vsync_front_porch = 8;
        config.timings.flags.pclk_active_neg = 1;
        config.data_width = 16;
        config.bits_per_pixel = 16;
        config.hsync_gpio_num = 46;
        config.vsync_gpio_num = 3;
        config.de_gpio_num = 5;
        config.pclk_gpio_num = 7;
        config.disp_gpio_num = -1;
        static const int DATA[] = {14, 38, 18, 17, 10, 39, 0, 45,
                                   48, 47, 21, 1, 2, 42, 41, 40};
        for (int i = 0; i < 16; ++i) config.data_gpio_nums[i] = DATA[i];
        // Anti-underrun configuration (the owner's "dancing horizontal
        // lines on the left edge" = starved bounce-buffer refills):
        // 20 rows per DRAM bounce buffer, an even divisor of the 480-row
        // frame. At Waveshare's own 16 MHz PCLK the panel drains one
        // buffer in 1 ms, twice the refill margin of the vendor's
        // WIDTH*10 default, so Wi-Fi/flash PSRAM bus contention cannot
        // starve a row start. The frame buffer stays in PSRAM; the
        // 64-byte DMA burst is the S3 GDMA maximum; the core's
        // CONFIG_LCD_RGB_RESTART_IN_VSYNC=y makes any residual underrun
        // self-heal at the next VSYNC instead of persisting as a shift.
        config.bounce_buffer_size_px = WIDTH * 20;
        config.dma_burst_size = 64;
        config.flags.fb_in_psram = 1;
        if (esp_lcd_new_rgb_panel(&config, &panel_) != ESP_OK) return false;
        if (esp_lcd_panel_init(panel_) != ESP_OK) { stop(); return false; }
        clear(0);
        return true;
    }

    // disp_on_off only controls a DISP GPIO (absent here), not RGB DMA.
    // Delete releases DMA, ISR, clocks and buffers; begin recreates them.
    bool stop() {
        if(!panel_) return true;
        if(esp_lcd_panel_del(panel_)!=ESP_OK) return false;
        panel_=nullptr;
        return true;
    }

    // Copy a full 800x480 RGB565 image into the panel's frame buffer.
    // ESP-IDF's draw_bitmap end coordinates are EXCLUSIVE (x_end = x_start +
    // width, y_end = y_start + height) and the source is read packed at
    // (x_end - x_start) pixels per row, so the full screen is exactly
    // (0, 0, WIDTH, HEIGHT). Passing WIDTH-1/HEIGHT-1 would copy a
    // (WIDTH-1)x(HEIGHT-1) region at a (WIDTH-1)-pixel stride, shearing each
    // row one pixel further left — the diagonal wrap this class must never
    // regress into (pinned by test_lcd43b_panel).
    // For an RGB panel this is a synchronous memcpy into PSRAM.
    bool blit(const uint16_t* pixels) {
        return panel_ && esp_lcd_panel_draw_bitmap(panel_, 0, 0, WIDTH, HEIGHT,
                                                   pixels) == ESP_OK;
    }

    // The caller owns PSRAM staging. Sources (cache/composited readout) remain
    // undimmed so repeated minute redraws and wake cannot accumulate dimming.
    bool blitDimmed(const uint16_t* pixels, uint16_t* staging, uint8_t brightness) {
        lcd::dimRgb565(pixels,staging,size_t(WIDTH)*HEIGHT,brightness);
        return blit(staging);
    }
    bool blitRegionDimmed(int x, int y, int w, int h, const uint16_t* pixels,
                          uint16_t* staging, uint8_t brightness) {
        if(x<0 || y<0 || w<=0 || h<=0 || x+w>WIDTH || y+h>HEIGHT) return false;
        lcd::dimRgb565(pixels,staging,size_t(w)*h,brightness);
        return blitRegion(x,y,w,h,staging);
    }

    // Packed regional source, exclusive end coordinates (same contract as blit).
    bool blitRegion(int x, int y, int w, int h, const uint16_t* pixels) {
        if(x<0 || y<0 || w<=0 || h<=0 || x+w>WIDTH || y+h>HEIGHT) return false;
        return panel_ && esp_lcd_panel_draw_bitmap(panel_,x,y,x+w,y+h,pixels)==ESP_OK;
    }

    // This board streams a single cached framebuffer through CPU-filled
    // bounce buffers. Compose into that buffer to avoid another full
    // PSRAM copy through draw_bitmap.
    uint16_t* framebuffer() const {
        if (!panel_) return nullptr;
        void* buffer = nullptr;
        if (esp_lcd_rgb_panel_get_frame_buffer(panel_, 1, &buffer) != ESP_OK)
            return nullptr;
        return static_cast<uint16_t*>(buffer);
    }

    void clear(uint16_t colour) {
        auto* out = reinterpret_cast<uint32_t*>(framebuffer());
        if (!out) return;
        const size_t words = size_t(WIDTH) * HEIGHT / 2;
        uint32_t fill = uint32_t(colour) << 16 | colour;
        for (size_t i = 0; i < words; ++i) out[i] = fill;
    }

    // The esp_lcd handle, for diagnostics that talk to the driver
    // directly (e.g. registering a VSYNC callback to measure underruns).
    esp_lcd_panel_handle_t handle() const { return panel_; }

private:
    esp_lcd_panel_handle_t panel_ = nullptr;
};
