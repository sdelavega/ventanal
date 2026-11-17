// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
#pragma once

// Host-test fake of the esp_lcd panel-ops interface, mirroring the pieces
// lcd43b/panel.h uses. Only C constructs: panel.h includes this inside
// `extern "C"`. The semantics that matter for the tests live in the fake
// implementations defined by the test itself.

#include <stdint.h>

typedef int esp_err_t;

#define ESP_OK 0
#define ESP_ERR_INVALID_ARG 0x02
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_ERR_NO_MEM 0x101

typedef struct esp_lcd_panel_t* esp_lcd_panel_handle_t;

esp_err_t esp_lcd_panel_init(esp_lcd_panel_handle_t panel);

// Documented ESP-IDF contract: x_start/y_start are included, x_end/y_end are
// NOT (the region is [x_start, x_end) x [y_start, y_end)) and color_data is
// packed with a (x_end - x_start) pixel stride.
esp_err_t esp_lcd_panel_draw_bitmap(esp_lcd_panel_handle_t panel, int x_start,
                                    int y_start, int x_end, int y_end,
                                    const void* color_data);

esp_err_t esp_lcd_panel_del(esp_lcd_panel_handle_t panel);
