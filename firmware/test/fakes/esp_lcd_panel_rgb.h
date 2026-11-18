// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
#pragma once

// Host-test fake of the esp_lcd RGB-panel interface, mirroring the config
// struct fields lcd43b/panel.h sets (field names copied from the ESP-IDF
// header so the production header compiles unchanged) plus the two entry
// points the sketch calls.

#include <stdint.h>
#include <stddef.h>

#include "esp_lcd_panel_ops.h"

typedef enum {
    LCD_CLK_SRC_DEFAULT = 0,
} lcd_clock_source_t;

typedef struct {
    uint32_t pclk_hz;
    uint32_t h_res;
    uint32_t v_res;
    uint32_t hsync_pulse_width;
    uint32_t hsync_back_porch;
    uint32_t hsync_front_porch;
    uint32_t vsync_pulse_width;
    uint32_t vsync_back_porch;
    uint32_t vsync_front_porch;
    struct {
        uint32_t hsync_idle_low : 1;
        uint32_t vsync_idle_low : 1;
        uint32_t de_idle_high : 1;
        uint32_t pclk_active_neg : 1;
        uint32_t pclk_idle_high : 1;
    } flags;
} esp_lcd_rgb_timing_t;

typedef struct {
    lcd_clock_source_t clk_src;
    esp_lcd_rgb_timing_t timings;
    size_t num_fbs;
    size_t data_width;
    size_t bits_per_pixel;
    size_t bounce_buffer_size_px;
    size_t dma_burst_size;
    int hsync_gpio_num;
    int vsync_gpio_num;
    int de_gpio_num;
    int pclk_gpio_num;
    int disp_gpio_num;
    int data_gpio_nums[16];
    struct {
        uint32_t disp_active_low : 1;
        uint32_t refresh_on_demand : 1;
        uint32_t fb_in_psram : 1;
    } flags;
} esp_lcd_rgb_panel_config_t;

esp_err_t esp_lcd_new_rgb_panel(const esp_lcd_rgb_panel_config_t* config,
                                esp_lcd_panel_handle_t* ret_panel);

esp_err_t esp_lcd_rgb_panel_get_frame_buffer(esp_lcd_panel_handle_t panel,
                                             uint32_t fb_num, void** fb0, ...);

typedef struct { int unused; } esp_lcd_rgb_panel_event_data_t;
typedef bool (*esp_lcd_rgb_panel_vsync_cb_t)(esp_lcd_panel_handle_t,
    const esp_lcd_rgb_panel_event_data_t*, void*);
typedef struct {
    esp_lcd_rgb_panel_vsync_cb_t on_vsync;
    esp_lcd_rgb_panel_vsync_cb_t on_frame_buf_complete;
} esp_lcd_rgb_panel_event_callbacks_t;

esp_err_t esp_lcd_rgb_panel_register_event_callbacks(esp_lcd_panel_handle_t panel,
    const esp_lcd_rgb_panel_event_callbacks_t* callbacks, void* context);
