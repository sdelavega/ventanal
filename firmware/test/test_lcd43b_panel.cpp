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
#include <cstdarg>
#include <vector>

namespace {

constexpr int FB_W = Panel::WIDTH;   // 800
constexpr int FB_H = Panel::HEIGHT;  // 480

uint16_t fb[size_t(FB_W) * FB_H];   // the fake panel's frame buffer
uint16_t back[size_t(FB_W) * FB_H];
uint16_t src[size_t(FB_W) * FB_H];  // a full-frame source image

struct BitmapCall {
    int x_start, y_start, x_end, y_end;
    const uint16_t* data;
};
std::vector<BitmapCall> calls;

unsigned newPanelCalls = 0, deleteCalls = 0;
bool deleteFails=false;
esp_lcd_rgb_panel_config_t lastConfig = {};

esp_lcd_rgb_panel_event_callbacks_t events{};
void* eventsContext = nullptr;
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

esp_err_t esp_lcd_panel_del(esp_lcd_panel_handle_t) {
    ++deleteCalls;
    return deleteFails?ESP_ERR_INVALID_STATE:ESP_OK;
}

esp_err_t esp_lcd_panel_init(esp_lcd_panel_handle_t) { return ESP_OK; }

esp_err_t esp_lcd_rgb_panel_register_event_callbacks(esp_lcd_panel_handle_t,
    const esp_lcd_rgb_panel_event_callbacks_t* callbacks, void* context) {
    events = *callbacks;
    eventsContext = context;
    return ESP_OK;
}

esp_err_t esp_lcd_rgb_panel_get_frame_buffer(esp_lcd_panel_handle_t, uint32_t count,
                                             void** fb0, ...) {
    *fb0 = fb;
    if (count == 2) {
        va_list args;
        va_start(args, fb0);
        *va_arg(args, void**) = back;
        va_end(args);
    }
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
    if (lastConfig.num_fbs == 2 && (color_data == back || color_data == fb))
        return ESP_OK; // driver-owned full frames switch at a scan boundary
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
        assert(panel.blit(panel.framebuffer()));
        assert(calls.size() == 1); // in-place composition needs no driver copy
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

    {
        Panel panel; assert(panel.begin([](bool) { return true; }));
        std::fill(fb,fb+FB_W*FB_H,0x1234);
        std::vector<uint16_t> region(250*80,0xabcd);
        assert(panel.blitRegion(550,0,250,80,region.data()));
        assert(calls.back().x_start==550 && calls.back().x_end==800 && calls.back().y_end==80);
        for(int y=0;y<FB_H;++y) for(int x=0;x<FB_W;++x)
            assert(fb[y*FB_W+x]==(x>=550 && y<80?0xabcd:0x1234));
        auto n=calls.size(); assert(!panel.blitRegion(550,0,251,80,region.data())); assert(calls.size()==n);
    }
    {
        Panel panel; assert(panel.begin([](bool) { return true; }));
        std::vector<uint16_t> source(FB_W*FB_H,0xffff), staging(source.size());
        assert(panel.blitDimmed(source.data(),staging.data(),25));
        assert(source[0]==0xffff);
        for(int i=0;i<FB_W*FB_H;++i) assert(fb[i]==0x39e7);
        std::vector<uint16_t> region(250*80,0xffff), scratch(region.size());
        assert(panel.blitRegionDimmed(550,0,250,80,region.data(),scratch.data(),25));
        for(int i=0;i<FB_W*FB_H;++i) assert(fb[i]==0x39e7); // no brighter clock rectangle
        assert(region[0]==0xffff);
        assert(panel.blitRegionDimmed(550,0,250,80,region.data(),scratch.data(),25));
        assert(fb[550]==0x39e7); // repeated redraw does not compound dimming
    }
    {
        Panel panel; assert(panel.begin([](bool) { return true; }));
        auto handle=panel.handle();
        deleteFails=true; assert(!panel.stop()); assert(panel.handle()==handle);
        deleteFails=false; assert(panel.stop()); assert(!panel.handle());
        const auto deletes=deleteCalls; assert(panel.stop()); assert(deleteCalls==deletes);
        assert(!panel.blit(src));
        assert(!panel.blitRegion(0,0,1,1,src));
        assert(panel.begin([](bool) { return true; }));
        assert(panel.blitDimmed(src,src,25));
    }
    {
        Panel panel;
        assert(panel.begin([](bool) { return true; }, 2));
        assert(lastConfig.num_fbs == 2 && panel.bufferCount() == 2);
        assert(panel.framebuffer() == back);
        const auto oldFirst = fb[0];
        assert(panel.blit(src) && calls.back().data == back && fb[0] == oldFirst);
        assert(panel.framebuffer() == nullptr); // old front is still in use
        events.on_vsync(panel.handle(), nullptr, eventsContext);
        assert(panel.framebuffer() == nullptr);
        events.on_vsync(panel.handle(), nullptr, eventsContext);
        assert(panel.framebuffer() == nullptr); // scan timing alone is insufficient
        events.on_frame_buf_complete(panel.handle(), nullptr, eventsContext);
        assert(panel.framebuffer() == fb);

        uint16_t clock = 0x4321;
        assert(panel.blitRegion(4, 3, 1, 1, &clock));
        assert(calls.back().data == fb && fb[3*FB_W+4] == clock);
        assert(fb[0] == back[0] && fb[FB_W*FB_H-1] == back[FB_W*FB_H-1]);
        events.on_frame_buf_complete(panel.handle(), nullptr, eventsContext);
        events.on_vsync(panel.handle(), nullptr, eventsContext);
        events.on_vsync(panel.handle(), nullptr, eventsContext);
        clock = 0x1234;
        assert(panel.blitRegion(4, 3, 1, 1, &clock));
        assert(calls.back().data == back && back[3*FB_W+4] == clock);
    }
    {
        Panel panel;
        assert(panel.begin([](bool) { return true; }));
        std::vector<uint8_t> wire(16 + Panel::Bytes);
        for (size_t i = 0; i < Panel::Bytes; ++i) wire[16+i] = uint8_t(i % 251);
        panel.copyBackground(panel.framebuffer(), wire.data() + 16);
        assert(memcmp(fb, wire.data() + 16, Panel::Bytes) == 0);
    }
    puts("lcd43b panel: blit args (0,0,800,480), vertical lines, full cover,"
         " anti-flicker bounce config passed");
    return 0;
}
