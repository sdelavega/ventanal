// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega

// Panel blit contract against a fake esp_lcd. ESP-IDF documents
// esp_lcd_panel_draw_bitmap's end coordinates as EXCLUSIVE (the region is
// [x_start, x_end) x [y_start, y_end)) and reads the source packed with a
// (x_end - x_start) pixel stride. A full 800x480 blit must therefore pass
// (0, 0, 800, 480): (0, 0, 799, 479) copies 799x479 pixels at a 799-pixel
// stride, shearing every row one pixel further and leaving the last row and
// column stale — the diagonal wrap seen on the owner's board.

#include "../lcd43b/panel.h"

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <vector>

namespace {

constexpr int FB_W = Panel::WIDTH;   // 800
constexpr int FB_H = Panel::HEIGHT;  // 480

uint16_t fb[size_t(FB_W) * FB_H];   // the fake panel's frame buffer
uint16_t src[size_t(FB_W) * FB_H];  // a full-frame source image

struct BitmapCall {
    int x_start, y_start, x_end, y_end;
    const uint16_t* data;
};
std::vector<BitmapCall> calls;

unsigned newPanelCalls = 0;
esp_lcd_rgb_panel_config_t lastConfig = {};

int dummyPanel = 0;   // the fake handle's pointee

}  // namespace

extern "C" {

esp_err_t esp_lcd_new_rgb_panel(const esp_lcd_rgb_panel_config_t* config,
                                esp_lcd_panel_handle_t* ret_panel) {
    if (!config || !ret_panel) return ESP_ERR_INVALID_ARG;
    ++newPanelCalls;
    lastConfig = *config;
    *ret_panel = reinterpret_cast<esp_lcd_panel_handle_t>(&dummyPanel);
    return ESP_OK;
}

esp_err_t esp_lcd_panel_init(esp_lcd_panel_handle_t) { return ESP_OK; }

esp_err_t esp_lcd_rgb_panel_get_frame_buffer(esp_lcd_panel_handle_t, uint32_t,
                                             void** fb0, ...) {
    *fb0 = fb;
    return ESP_OK;
}

// Model of the documented driver behaviour: bounds-check like the real
// driver, record the exact arguments, and copy the [x_start, x_end) x
// [y_start, y_end) region from a source packed at (x_end - x_start) pixels
// per row into the frame buffer.
esp_err_t esp_lcd_panel_draw_bitmap(esp_lcd_panel_handle_t, int x_start,
                                    int y_start, int x_end, int y_end,
                                    const void* color_data) {
    if (!color_data || x_start >= x_end || y_start >= y_end || x_start < 0
        || y_start < 0 || x_end > int(lastConfig.timings.h_res)
        || y_end > int(lastConfig.timings.v_res))
        return ESP_ERR_INVALID_ARG;
    calls.push_back({x_start, y_start, x_end, y_end,
                     static_cast<const uint16_t*>(color_data)});
    const int w = x_end - x_start;
    const int h = y_end - y_start;
    const uint16_t* from = static_cast<const uint16_t*>(color_data);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            fb[size_t(y_start + y) * FB_W + (x_start + x)] = from[size_t(y) * w + x];
    return ESP_OK;
}

}  // extern "C"

int main() {
    {   // begin() configures the full 800x480 RGB geometry.
        Panel panel;
        assert(panel.begin([](bool) { return true; }));
        assert(newPanelCalls == 1);
        assert(lastConfig.timings.h_res == 800);
        assert(lastConfig.timings.v_res == 480);
        assert(lastConfig.data_width == 16);
        assert(lastConfig.bits_per_pixel == 16);

        // Audit: clear() fills the entire frame buffer, including the last
        // row and column.
        std::fill(fb, fb + size_t(FB_W) * FB_H, uint16_t(0xABCD));
        panel.clear(0);
        for (size_t i = 0; i < size_t(FB_W) * FB_H; ++i) assert(fb[i] == 0);
    }

    {   // blit() must pass the exact full-frame coordinates: ESP-IDF's
        // x_end/y_end are exclusive, so the full screen is (0, 0, 800, 480).
        calls.clear();
        Panel panel;
        assert(panel.begin([](bool) { return true; }));
        assert(panel.blit(src));
        assert(calls.size() == 1);
        assert(calls[0].x_start == 0);
        assert(calls[0].y_start == 0);
        assert(calls[0].x_end == FB_W);
        assert(calls[0].y_end == FB_H);
        assert(calls[0].data == src);
    }

    {   // Test pattern: white vertical lines at x=0 and x=799 on black stay
        // vertical through the blit, every row (including the last) and
        // column is delivered, and no wrap-around shears any other pixel.
        calls.clear();
        Panel panel;
        assert(panel.begin([](bool) { return true; }));
        std::fill(fb, fb + size_t(FB_W) * FB_H, uint16_t(0xABCD));  // stale sentinel
        std::fill(src, src + size_t(FB_W) * FB_H, uint16_t(0x0000));
        for (int y = 0; y < FB_H; ++y) {
            src[size_t(y) * FB_W] = 0xFFFF;              // line at x=0
            src[size_t(y) * FB_W + (FB_W - 1)] = 0xFFFF; // line at x=799
        }
        assert(panel.blit(src));
        assert(calls.size() == 1);
        for (int y = 0; y < FB_H; ++y) {
            for (int x = 0; x < FB_W; ++x) {
                const uint16_t pixel = fb[size_t(y) * FB_W + x];
                if (x == 0 || x == FB_W - 1) assert(pixel == 0xFFFF);
                else assert(pixel == 0x0000);
            }
        }
        // The old (0, 0, 799, 479) call would be rejected by neither bound;
        // pin the geometry once more so a regression cannot hide there.
        assert(calls[0].x_end - calls[0].x_start == FB_W);
        assert(calls[0].y_end - calls[0].y_start == FB_H);
    }

    {   // Anti-flicker configuration (fix-r3-flicker.md): the DRAM bounce
        // buffers are enlarged to WIDTH*20 pixels — 20 rows per buffer, an
        // even divisor of the 480-row frame — so each PSRAM refill window
        // spans 1 ms of panel drain instead of 0.5 ms and Wi-Fi/flash bus
        // contention can no longer starve a row refill (the owner's
        // dancing lines on the left edge). PCLK stays at Waveshare's own
        // 16 MHz board value, the DMA burst stays at 64 bytes (the S3
        // GDMA maximum), and the frame buffer stays in PSRAM. The core's
        // CONFIG_LCD_RGB_RESTART_IN_VSYNC=y makes any residual underrun
        // self-heal at the next VSYNC instead of persisting.
        Panel panel;
        assert(panel.begin([](bool) { return true; }));
        assert(lastConfig.bounce_buffer_size_px == size_t(FB_W) * 20);
        assert(lastConfig.bounce_buffer_size_px % size_t(FB_W) == 0);
        assert(size_t(FB_W) * FB_H % lastConfig.bounce_buffer_size_px == 0);
        assert(lastConfig.dma_burst_size == 64);
        assert(lastConfig.timings.pclk_hz == 16 * 1000 * 1000);
        assert(lastConfig.flags.fb_in_psram == 1);
    }

    puts("lcd43b panel: blit args (0,0,800,480), vertical lines, full cover,"
         " anti-flicker bounce config passed");
    return 0;
}
