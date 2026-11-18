// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega

#include "panel.h"

#if defined(CONFIG_IDF_TARGET_ESP32S3)
#include "esp32s3/rom/cache.h"
#ifdef VENTANAL_LCD_DIAG_PANEL
#include "soc/gdma_channel.h"
#include "soc/gdma_reg.h"
#include "soc/gdma_struct.h"
#endif
#ifdef VENTANAL_LCD_DIAG_PANEL_STALL
#include "esp_rom_sys.h"
#endif
#endif

bool Panel::begin(bool (*setLcdReset)(bool), esp_lcd_rgb_panel_vsync_cb_t observer) {
    if (panel_) return true;
    setLcdReset(true);
    delay(10);
    setLcdReset(false);
    delay(100);
    esp_lcd_rgb_panel_config_t config{};
    config.clk_src = LCD_CLK_SRC_DEFAULT;
    config.timings.pclk_hz = Pclk;
    config.timings.h_res = WIDTH;
    config.timings.v_res = HEIGHT;
    config.timings.hsync_pulse_width = 4;
    config.timings.hsync_back_porch = 8;
    config.timings.hsync_front_porch = 8;
    config.timings.vsync_pulse_width = VSyncPulse;
    config.timings.vsync_back_porch = VBackPorch;
    config.timings.vsync_front_porch = VFrontPorch;
    config.timings.flags.pclk_active_neg = 1;
    config.data_width = config.bits_per_pixel = 16;
    config.hsync_gpio_num = 46;
    config.vsync_gpio_num = 3;
    config.de_gpio_num = 5;
    config.pclk_gpio_num = 7;
    config.disp_gpio_num = -1;
    const int data[] = {14, 38, 18, 17, 10, 39, 0, 45, 48, 47, 21, 1, 2, 42, 41, 40};
    for (int i = 0; i < 16; ++i) config.data_gpio_nums[i] = data[i];
    config.bounce_buffer_size_px = WIDTH * BounceRows;
    config.dma_burst_size = 64;
    config.flags.no_fb = 1;
    if (esp_lcd_new_rgb_panel(&config, &panel_) != ESP_OK) return false;
    observer_ = observer;
    scenes_[0] = scenes_[1] = {};
    submitted_ = 0;
    armed_.store(0, std::memory_order_relaxed);
    requested_.store(0, std::memory_order_relaxed);
    active_.store(0, std::memory_order_relaxed);
#ifdef VENTANAL_LCD_DIAG_PANEL
    syncAt_ = 0;
    dmaChannel_ = -1;
#if defined(CONFIG_IDF_TARGET_ESP32S3)
    for (unsigned i = 0; i < sizeof(GDMA.channel) / sizeof(GDMA.channel[0]); ++i) {
        if (GDMA.channel[i].out.peri_sel.sel == SOC_GDMA_TRIG_PERIPH_LCD0) {
            dmaChannel_ = i;
            break;
        }
    }
#endif
    diag_.resetCadence();
    lcd::log("bounce dma channel=%d\n", dmaChannel_);
#endif
    esp_lcd_rgb_panel_event_callbacks_t callbacks{};
    callbacks.on_vsync = vsync;
    callbacks.on_bounce_empty = fillBounce;
    if (esp_lcd_rgb_panel_register_event_callbacks(panel_, &callbacks, this) != ESP_OK ||
        esp_lcd_panel_init(panel_) != ESP_OK) {
        stop();
        return false;
    }
    return true;
}

bool Panel::stop() {
    if (!panel_) return true;
    if (esp_lcd_panel_del(panel_) != ESP_OK) return false;
    panel_ = nullptr;
    return true;
}

bool Panel::waitForVsync() const {
    if (!panel_) return false;
    const auto previous = vsyncs_.load(std::memory_order_relaxed);
    const uint32_t started = millis();
    while (vsyncs_.load(std::memory_order_relaxed) == previous) {
        if (millis() - started >= 120) return false;
        delay(1);
    }
    return true;
}

bool Panel::waitForPresent() const {
    const uint32_t started = millis();
    while (active_.load(std::memory_order_acquire) != unsigned(submitted_)) {
        if (millis() - started >= 120) return false;
        delay(1);
    }
    return true;
}

bool Panel::references(const uint16_t* pixels) const {
    if (!panel_ || !pixels) return false;
    const unsigned active = active_.load(std::memory_order_acquire);
    return scenes_[active].pixels == pixels || scenes_[submitted_].pixels == pixels;
}

void Panel::present(int slot, lcd::ScanFrame scene, uint8_t brightness) {
    scene.brightnessQuarters = brightness / 25;
    scenes_[slot] = scene;
    submitted_ = slot;
    requested_.store(slot, std::memory_order_release);
}

#ifdef VENTANAL_LCD_DIAG_PANEL
__attribute__((always_inline)) inline void Panel::sampleDma() {
#if defined(CONFIG_IDF_TARGET_ESP32S3)
    if (dmaChannel_ < 0) return;
    auto& channel = GDMA.channel[dmaChannel_].out;
    constexpr uint32_t fifoFlags = GDMA_OUTFIFO_UDF_L1_CH0_INT_RAW_M |
                                   GDMA_OUTFIFO_UDF_L3_CH0_INT_RAW_M;
    const uint32_t status = channel.int_raw.val;
    if (status & fifoFlags) channel.int_clr.val = status & fifoFlags;
    diag_.dmaStatus(status & fifoFlags, status & GDMA_OUT_DSCR_ERR_CH0_INT_RAW_M);
#endif
}
#endif

bool IRAM_ATTR Panel::vsync(esp_lcd_panel_handle_t handle,
                           const esp_lcd_rgb_panel_event_data_t* event, void* context) {
    auto& self = *static_cast<Panel*>(context);
    self.armed_.store(self.requested_.load(std::memory_order_acquire), std::memory_order_release);
    self.vsyncs_.fetch_add(1, std::memory_order_relaxed);
#ifdef VENTANAL_LCD_DIAG_PANEL
    self.syncAt_ = uint32_t(esp_timer_get_time());
    self.diag_.vsync(self.syncAt_, VBlankUs, HEIGHT / BounceRows);
    self.sampleDma();
#endif
    return self.observer_ && self.observer_(handle, event, nullptr);
}

bool IRAM_ATTR Panel::fillBounce(esp_lcd_panel_handle_t, void* buffer, int pos,
                               int bytes, void* context) {
    auto& self = *static_cast<Panel*>(context);
#ifdef VENTANAL_LCD_DIAG_PANEL
    const uint32_t start = uint32_t(esp_timer_get_time());
    const bool prefill = self.syncAt_ && start - self.syncAt_ < 500 &&
        pos < 2 * WIDTH * BounceRows;
    const bool lastActive = !prefill && pos == WIDTH * BounceRows;
    self.diag_.beginFill(start, lastActive);
#ifdef VENTANAL_LCD_DIAG_PANEL_STALL
    if (self.diag_.inject(lastActive)) esp_rom_delay_us(3000);
#endif
#endif
    // The driver may prefetch row zero before the physical VSYNC. Changing
    // source here (after VSYNC has armed it) keeps both prefetched buffers
    // and every subsequent row on the same source.
    if (pos == 0)
        self.active_.store(self.armed_.load(std::memory_order_acquire), std::memory_order_release);
    const auto& scene = self.scenes_[self.active_.load(std::memory_order_relaxed)];
    if (scene.pixels) memcpy(buffer, scene.pixels + pos, bytes);
    else memset(buffer, 0, bytes);
    lcd::finishScanRows(static_cast<uint16_t*>(buffer), scene, WIDTH, pos, bytes);
#if defined(CONFIG_IDF_TARGET_ESP32S3)
    // Warm the first cache lines of the next section while DMA drains this
    // one, leaving cache space for other work and staying inside the frame.
    constexpr unsigned BounceBytes = WIDTH * BounceRows * sizeof(uint16_t);
    constexpr unsigned PreloadBytes = BounceBytes < 24 * 1024 ? BounceBytes : 24 * 1024;
    const int next = pos + bytes / sizeof(uint16_t);
    const auto* pixels = next < WIDTH * HEIGHT ? scene.pixels :
        self.scenes_[self.armed_.load(std::memory_order_relaxed)].pixels;
    if (pixels)
        Cache_Start_DCache_Preload(uint32_t(uintptr_t(pixels + next % (WIDTH * HEIGHT))),
                                  PreloadBytes, 0);
#endif
#ifdef VENTANAL_LCD_DIAG_PANEL
    self.sampleDma();
    self.diag_.fill(start, pos, WIDTH, HEIGHT, BounceRows, DrainUs, VBlankUs,
                    prefill);
#endif
    return false;
}
