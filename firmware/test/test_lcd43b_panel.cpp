// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega

#include "../lcd43b/panel.h"
#include "../lcd43b/settings.h"

#include <array>
#include <cassert>
#include <cstdio>
#include <memory>
#include <vector>

namespace {
esp_lcd_rgb_panel_config_t cfg{};
esp_lcd_rgb_panel_event_callbacks_t events{};
void* context = nullptr;
int dummy = 0;
bool deleteFails = false;
constexpr int Width = Panel::WIDTH;
constexpr int Height = Panel::HEIGHT;
constexpr int BouncePixels = Width * Panel::BounceRows;
} // namespace

extern "C" {
esp_err_t esp_lcd_new_rgb_panel(const esp_lcd_rgb_panel_config_t* config,
                               esp_lcd_panel_handle_t* out) {
    cfg = *config;
    *out = reinterpret_cast<esp_lcd_panel_handle_t>(&dummy);
    return ESP_OK;
}

esp_err_t esp_lcd_panel_del(esp_lcd_panel_handle_t) {
    return deleteFails ? ESP_ERR_INVALID_STATE : ESP_OK;
}

esp_err_t esp_lcd_panel_init(esp_lcd_panel_handle_t) { return ESP_OK; }

esp_err_t esp_lcd_rgb_panel_register_event_callbacks(esp_lcd_panel_handle_t,
    const esp_lcd_rgb_panel_event_callbacks_t* callbacks, void* arg) {
    events = *callbacks;
    context = arg;
    return ESP_OK;
}
} // extern "C"

int main() {
    auto panel = std::make_unique<Panel>();
    assert(panel->begin([](bool) { return true; }));
    assert(cfg.flags.no_fb && cfg.num_fbs == 0 && events.on_bounce_empty);
    assert(cfg.timings.h_res == Width && cfg.timings.v_res == Height);
    assert(cfg.timings.pclk_hz == Panel::Pclk && cfg.bits_per_pixel == 16);
    assert(cfg.bounce_buffer_size_px == BouncePixels && cfg.dma_burst_size == 64);
    assert(Width * Height % BouncePixels == 0);

    std::vector<uint16_t> source(Width * Height), second(Width * Height, 0xabcd);
    std::vector<uint16_t> bounce(BouncePixels);
    for (int i = 0; i < Width * Height; ++i) source[i] = 1 + i % 65534;
    const auto refill = [&](int offset) {
        events.on_bounce_empty(panel->handle(), bounce.data(), offset,
                               bounce.size() * sizeof(bounce[0]), context);
    };
    const auto vsync = [&] { events.on_vsync(panel->handle(), nullptr, context); };

    const std::array<uint16_t, 6> patch = {90, 91, 100, 101, 110, 111};
    lcd::ScanFrame view{};
    view.pixels = source.data();
    view.patches[0] = {{Width - 2, Panel::BounceRows - 1, 2, 3}, patch.data()};
    assert(panel->prepare() == 1);
    panel->present(1, view, 100);
    refill(0); // row zero already prefetched before VSYNC
    assert(bounce.front() == 0);
    vsync();
    refill(BouncePixels);
    assert(bounce.front() == 0); // mid-frame refills still use the old scene
    refill(0);
    assert(panel->waitForPresent() && panel->references(source.data()));
    assert(bounce.front() == source.front());
    assert(bounce[BouncePixels - 3] == source[BouncePixels - 3]);
    assert(bounce[BouncePixels - 2] == 90 && bounce.back() == 91);
    refill(BouncePixels);
    assert(bounce[Width - 2] == 100 && bounce[Width - 1] == 101);
    assert(bounce[Width * 2 - 2] == 110 && bounce[Width * 2 - 1] == 111);
    assert(bounce[Width * 3 - 1] == source[BouncePixels + Width * 3 - 1]);
    refill(Width * Height - BouncePixels);
    assert(bounce.front() == source[Width * Height - BouncePixels]);
    assert(bounce.back() == source.back());

    assert(panel->prepare() == 0);
    panel->present(0, {second.data()}, 100);
    vsync();
    refill(Width * Height - BouncePixels);
    assert(panel->references(source.data()) && bounce.back() == source.back());
    refill(0);
    assert(!panel->references(source.data()) && panel->references(second.data()));
    assert(bounce.front() == second.front() && bounce.back() == second.back());

    // Compare the exact RGB565 scaling for all possible input colours.
    std::vector<uint16_t> colours(65536), expected(65536);
    for (unsigned i = 0; i < colours.size(); ++i) colours[i] = i;
    for (unsigned n = 1; n <= 3; ++n) {
        lcd::dimRgb565(colours.data(), expected.data(), colours.size(), n * 25);
        for (unsigned i = 0; i < colours.size(); ++i) {
            const unsigned other = 65535 - i;
            const uint32_t pair = i | (other << 16);
            const uint32_t scaled = expected[i] | (uint32_t(expected[other]) << 16);
            assert(lcd::dimScanPair(pair, n) == scaled);
        }
    }
    deleteFails = true;
    assert(!panel->stop() && panel->references(second.data()));
    deleteFails = false;
    assert(panel->stop() && !panel->references(second.data()));
    assert(panel->prepare() == -1);
    puts("lcd43b panel: bounce geometry, frame-boundary swaps, patch rows and RGB565 dimming passed");
}
