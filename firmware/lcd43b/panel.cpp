// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega

#include "panel.h"
#include <cassert>
#ifndef VENTANAL_HOST_TEST
#include <esp_timer.h>
#include <esp_intr_alloc.h>
#include <soc/interrupts.h>
#endif

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

#ifndef VENTANAL_HOST_TEST
#ifndef VENTANAL_LCD_ISR_LEVEL
#define VENTANAL_LCD_ISR_LEVEL 0
#endif

namespace {
std::atomic<TaskHandle_t> rgbAllocator{nullptr};
int rgbLevel = VENTANAL_LCD_ISR_LEVEL;
}

extern "C" esp_err_t __real_esp_intr_alloc_intrstatus(int, int, uint32_t, uint32_t,
                                                     intr_handler_t, void*, intr_handle_t*);
extern "C" esp_err_t __wrap_esp_intr_alloc_intrstatus(int source, int flags, uint32_t reg,
                                                     uint32_t mask, intr_handler_t handler,
                                                     void* arg, intr_handle_t* out) {
    const bool rgb = source == ETS_LCD_CAM_INTR_SOURCE ||
        (source >= ETS_DMA_OUT_CH0_INTR_SOURCE && source <= ETS_DMA_OUT_CH4_INTR_SOURCE);
    if (rgb && rgbLevel == 3 && rgbAllocator.load(std::memory_order_relaxed) == xTaskGetCurrentTaskHandle())
        flags = (flags & ~ESP_INTR_FLAG_LOWMED) | ESP_INTR_FLAG_LEVEL3;
    return __real_esp_intr_alloc_intrstatus(source, flags, reg, mask, handler, arg, out);
}

void Panel::setInterruptLevel(int level) { rgbLevel = level; }
#endif

#if defined(CONFIG_IDF_TARGET_ESP32S3)
extern "C" void lcdDimRgb565Vectors(uint16_t*, int, unsigned, const uint32_t*);

namespace {
alignas(16) DRAM_ATTR const uint32_t DimMasks[] = {
    0x7bef7bef, 0x7bef7bef, 0x7bef7bef, 0x7bef7bef,
    0x39e739e7, 0x39e739e7, 0x39e739e7, 0x39e739e7,
    0x08210821, 0x08210821, 0x08210821, 0x08210821,
    0x80008000, 0x80008000, 0x80008000, 0x80008000,
};

__attribute__((always_inline))
inline void dimDmaPixels(uint16_t* pixels, int count, unsigned quarters) {
    if (quarters >= 4) return;
    while (count && (reinterpret_cast<uintptr_t>(pixels) & 15)) {
        *pixels = uint16_t(lcd::dimScanPair(*pixels, quarters));
        ++pixels;
        --count;
    }
    const int vectors = count / 8;
    if (vectors) lcdDimRgb565Vectors(pixels, vectors, quarters, DimMasks);
    lcd::dimScanPixels(pixels + vectors * 8, count % 8, quarters);
}
} // namespace

#ifdef VENTANAL_LCD_DIAG_DIM
bool Panel::verifyDimming() {
    alignas(16) uint16_t test[16];
    for (unsigned q = 1; q < 4; ++q) {
        for (unsigned i = 0; i < 65536; i += 16) {
            for (unsigned j = 0; j < 16; ++j) test[j] = i + j;
            // Keep the scheduler out while the task borrows CP3.
            const auto level = portSET_INTERRUPT_MASK_FROM_ISR();
            dimDmaPixels(test, 16, q);
            portCLEAR_INTERRUPT_MASK_FROM_ISR(level);
            for (unsigned j = 0; j < 16; ++j) {
                const unsigned rgb = i + j;
                const unsigned expected = ((rgb >> 11) * q / 4 << 11)
                    | (((rgb >> 5) & 63) * q / 4 << 5) | ((rgb & 31) * q / 4);
                if (test[j] != expected) return false;
            }
        }
    }
    return true;
}
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
#ifndef VENTANAL_HOST_TEST
    rgbAllocator.store(xTaskGetCurrentTaskHandle(), std::memory_order_relaxed);
#endif
    const esp_err_t created = esp_lcd_new_rgb_panel(&config, &panel_);
#ifndef VENTANAL_HOST_TEST
    rgbAllocator.store(nullptr, std::memory_order_relaxed);
#endif
    if (created != ESP_OK) return false;
    lastVsyncUs_.store(0, std::memory_order_relaxed);
    lastEofUs_.store(0, std::memory_order_relaxed);
#ifdef VENTANAL_LCD_DIAG_F3
    lateRestarts_.store(0, std::memory_order_relaxed);
    shortFrames_.store(0, std::memory_order_relaxed);
    maxFillUs_.store(0, std::memory_order_relaxed);
    maxGapUs_.store(0, std::memory_order_relaxed);
#endif
    eofs_.store(0, std::memory_order_relaxed);
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
    assert(panel_ && slot == 1 - submitted_ &&
           active_.load(std::memory_order_acquire) == unsigned(submitted_));
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
#ifndef VENTANAL_HOST_TEST
    const uint32_t now = uint32_t(esp_timer_get_time());
    const uint32_t previousVsync = self.lastVsyncUs_.exchange(now, std::memory_order_relaxed);
    const bool shortFrame = self.eofs_.exchange(0, std::memory_order_relaxed) < HEIGHT / BounceRows;
    const uint32_t lastEof = self.lastEofUs_.exchange(0, std::memory_order_relaxed);
    if (previousVsync) {
        const uint32_t gap = now - previousVsync;
        if (gap > self.maxGapUs_.load(std::memory_order_relaxed))
            self.maxGapUs_.store(gap, std::memory_order_relaxed);
        if (shortFrame) self.shortFrames_.fetch_add(1, std::memory_order_relaxed);
#ifdef CONFIG_LCD_RGB_RESTART_IN_VSYNC
        const bool restarting = true;
#else
        const bool restarting = shortFrame;
#endif
        const uint32_t phase = now - lastEof;
        if (restarting && lastEof && phase > VBlankUs && phase < VBlankUs * 3)
            self.lateRestarts_.fetch_add(1, std::memory_order_relaxed);
    }
#endif
#ifdef VENTANAL_LCD_DIAG_PANEL
    self.syncAt_ = now;
    self.diag_.vsync(self.syncAt_, VBlankUs, HEIGHT / BounceRows);
    self.sampleDma();
#endif
    return self.observer_ && self.observer_(handle, event, nullptr);
}

bool IRAM_ATTR Panel::fillBounce(esp_lcd_panel_handle_t, void* buffer, int pos,
                               int bytes, void* context) {
    auto& self = *static_cast<Panel*>(context);
#ifndef VENTANAL_HOST_TEST
    const uint32_t start = uint32_t(esp_timer_get_time());
    const uint32_t lastVsync = self.lastVsyncUs_.load(std::memory_order_relaxed);
    const bool prefill = !lastVsync || (start - lastVsync < 500 && pos < 2 * WIDTH * BounceRows);
    const bool lastActive = !prefill && pos == WIDTH * BounceRows;
    if (!prefill) self.eofs_.fetch_add(1, std::memory_order_relaxed);
    if (lastActive) self.lastEofUs_.store(start, std::memory_order_relaxed);
#endif
#ifdef VENTANAL_LCD_DIAG_PANEL
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
    if (scene.local) scene.local->decode(static_cast<uint16_t*>(buffer), pos / WIDTH,
                                         bytes / (WIDTH * sizeof(uint16_t)));
    else if (scene.pixels) memcpy(buffer, scene.pixels + pos, bytes);
    else memset(buffer, 0, bytes);
    auto* pixelsOut = static_cast<uint16_t*>(buffer);
#if defined(CONFIG_IDF_TARGET_ESP32S3)
    lcd::finishScanRows(pixelsOut, scene, WIDTH, pos, bytes, false);
    dimDmaPixels(pixelsOut, bytes / sizeof(*pixelsOut), scene.brightnessQuarters);
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
#else
    lcd::finishScanRows(pixelsOut, scene, WIDTH, pos, bytes);
#endif
#ifndef VENTANAL_HOST_TEST
    const uint32_t elapsed = uint32_t(esp_timer_get_time()) - start;
    if (elapsed > self.maxFillUs_.load(std::memory_order_relaxed))
        self.maxFillUs_.store(elapsed, std::memory_order_relaxed);
#endif
#ifdef VENTANAL_LCD_DIAG_PANEL
    self.sampleDma();
    self.diag_.fill(start, pos, WIDTH, HEIGHT, BounceRows, DrainUs, VBlankUs,
                    prefill);
#endif
    return false;
}
