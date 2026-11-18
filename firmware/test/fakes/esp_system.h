// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
#pragma once
enum esp_reset_reason_t {
    ESP_RST_UNKNOWN, ESP_RST_POWERON, ESP_RST_EXT, ESP_RST_SW, ESP_RST_PANIC,
    ESP_RST_INT_WDT, ESP_RST_TASK_WDT, ESP_RST_WDT, ESP_RST_DEEPSLEEP,
    ESP_RST_BROWNOUT, ESP_RST_SDIO, ESP_RST_USB, ESP_RST_JTAG, ESP_RST_EFUSE,
    ESP_RST_PWR_GLITCH, ESP_RST_CPU_LOCKUP
};
inline esp_reset_reason_t fakeReset=ESP_RST_POWERON;
inline esp_reset_reason_t esp_reset_reason() { return fakeReset; }
#define ESP_ERROR_CHECK(x) do { if ((x)!=0) std::abort(); } while (false)

struct Restarting {};
inline void esp_restart() { throw Restarting{}; }
