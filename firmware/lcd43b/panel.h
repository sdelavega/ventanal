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
#include <cstring>
#include <atomic>
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
    bool begin(bool (*setLcdReset)(bool), unsigned buffers = 1,
               esp_lcd_rgb_panel_vsync_cb_t observer = nullptr) {
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
        config.num_fbs = buffers;
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
        observer_ = observer;
        front_ = 0;
        pending_ = false;
        regionWidth_ = 0;
        void *first = nullptr, *second = nullptr;
        const auto buffersStatus = buffers == 2
            ? esp_lcd_rgb_panel_get_frame_buffer(panel_, 2, &first, &second)
            : esp_lcd_rgb_panel_get_frame_buffer(panel_, 1, &first);
        buffers_[0] = static_cast<uint16_t*>(first);
        buffers_[1] = static_cast<uint16_t*>(second);
        esp_lcd_rgb_panel_event_callbacks_t callbacks = {};
        callbacks.on_vsync = vsync;
        callbacks.on_frame_buf_complete = frameComplete;
        if (buffersStatus != ESP_OK ||
            esp_lcd_rgb_panel_register_event_callbacks(panel_, &callbacks, this) != ESP_OK ||
            esp_lcd_panel_init(panel_) != ESP_OK) {
            stop();
            return false;
        }
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
    // Passing a driver-owned back buffer queues it for the next complete
    // frame in bounce mode. External pixels are copied into it first.
    void copyBackground(uint16_t* dst, const uint8_t* src) const {
        if (buffers_[1]) {
            memcpy(dst, src, Bytes);
            return;
        }
        // Separate PSRAM reads from writes on the shared bus. The tile
        // lives in internal task-stack RAM and spans one physical row.
        constexpr size_t tileBytes = WIDTH * sizeof(uint16_t);
        static_assert(Bytes % tileBytes == 0);
        alignas(16) uint8_t tile[tileBytes];
        auto* out = reinterpret_cast<uint8_t*>(dst);
        for (size_t offset = 0; offset < Bytes; offset += tileBytes) {
            memcpy(tile, src + offset, tileBytes);
            memcpy(out + offset, tile, tileBytes);
        }
    }

    bool blit(const uint16_t* pixels) {
        auto* target = framebuffer();
        if (!target || !pixels) return false;
        if (buffers_[1]) {
            if (pixels != target) memcpy(target, pixels, Bytes);
            return present(0, 0, WIDTH, HEIGHT);
        }
        return pixels == target ||
            esp_lcd_panel_draw_bitmap(panel_, 0, 0, WIDTH, HEIGHT, pixels) == ESP_OK;
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
        if (!panel_ || !pixels) return false;
        if (!buffers_[1])
            return esp_lcd_panel_draw_bitmap(panel_, x, y, x+w, y+h, pixels) == ESP_OK;

        auto* target = framebuffer();
        if (!target) return false;
        // After a full redraw the old back buffer holds a different screen.
        // Subsequent updates to the same clock rectangle need only its rows.
        if (x != regionX_ || y != regionY_ || w != regionWidth_ || h != regionHeight_)
            memcpy(target, buffers_[front_], Bytes);
        for (int row = 0; row < h; ++row)
            memcpy(target + (y+row)*WIDTH + x, pixels + row*w, size_t(w)*sizeof(*pixels));
        return present(x, y, w, h);
    }

    uint16_t* framebuffer() {
        if (!panel_ || !waitForPresent()) return nullptr;
        return buffers_[buffers_[1] ? 1 - front_ : 0];
    }

    bool waitForPresent() {
        if (!pending_) return true;
        const uint32_t started = millis();
        // The driver's bounce reader samples the submitted index at a
        // frame-complete boundary. Two subsequent VSYNCs also let any
        // already-filled bounce buffers drain before reusing the old front.
        while (completed_.load(std::memory_order_relaxed) == submittedFrame_ ||
               vsyncs_.load(std::memory_order_relaxed) - submittedVsync_ < 2) {
            if (millis() - started >= 120) return false;
            delay(1);
        }
        pending_ = false;
        return true;
    }

    uint32_t vsyncCount() const { return vsyncs_.load(std::memory_order_relaxed); }

    bool waitForVsync() const {
        const uint32_t previous = vsyncCount();
        const uint32_t started = millis();
        while (vsyncCount() == previous) {
            if (millis() - started >= 120) return false;
            delay(1);
        }
        return true;
    }

    unsigned bufferCount() const { return panel_ ? (buffers_[1] ? 2 : 1) : 0; }
    static constexpr size_t Bytes = size_t(WIDTH) * HEIGHT * sizeof(uint16_t);

    void clear(uint16_t colour) {
        auto* out = reinterpret_cast<uint32_t*>(framebuffer());
        if (!out) return;
        const size_t words = Bytes / sizeof(*out);
        uint32_t fill = uint32_t(colour) << 16 | colour;
        for (size_t i = 0; i < words; ++i) out[i] = fill;
        if (buffers_[1]) present(0, 0, WIDTH, HEIGHT);
    }

    // The esp_lcd handle, for diagnostics that talk to the driver
    // directly (e.g. registering a VSYNC callback to measure underruns).
    esp_lcd_panel_handle_t handle() const { return panel_; }

private:
    static bool IRAM_ATTR vsync(esp_lcd_panel_handle_t handle,
                               const esp_lcd_rgb_panel_event_data_t* event, void* context) {
        auto& self = *static_cast<Panel*>(context);
        self.vsyncs_.fetch_add(1, std::memory_order_relaxed);
        return self.observer_ && self.observer_(handle, event, nullptr);
    }

    static bool IRAM_ATTR frameComplete(esp_lcd_panel_handle_t,
                                       const esp_lcd_rgb_panel_event_data_t*, void* context) {
        auto& self = *static_cast<Panel*>(context);
        self.completed_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    bool present(int x, int y, int w, int h) {
        const unsigned back = 1 - front_;
        if (esp_lcd_panel_draw_bitmap(panel_, 0, 0, WIDTH, HEIGHT, buffers_[back]) != ESP_OK)
            return false;
        submittedFrame_ = completed_.load(std::memory_order_relaxed);
        submittedVsync_ = vsyncCount();
        pending_ = true;
        front_ = back;
        regionX_ = x;
        regionY_ = y;
        regionWidth_ = (w == WIDTH && h == HEIGHT) ? 0 : w;
        regionHeight_ = h;
        return true;
    }

    esp_lcd_panel_handle_t panel_ = nullptr;
    esp_lcd_rgb_panel_vsync_cb_t observer_ = nullptr;
    uint16_t* buffers_[2]{};
    std::atomic<uint32_t> vsyncs_{0}, completed_{0};
    uint32_t submittedVsync_ = 0, submittedFrame_ = 0;
    unsigned front_ = 0;
    bool pending_ = false;
    int regionX_ = 0, regionY_ = 0, regionWidth_ = 0, regionHeight_ = 0;
};
