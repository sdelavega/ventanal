// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
#pragma once
enum esp_reset_reason_t { ESP_RST_POWERON, ESP_RST_DEEPSLEEP, ESP_RST_SW,
    ESP_RST_TASK_WDT, ESP_RST_INT_WDT, ESP_RST_WDT };
inline esp_reset_reason_t fakeReset=ESP_RST_POWERON;
inline esp_reset_reason_t esp_reset_reason() { return fakeReset; }
#define ESP_ERROR_CHECK(x) do { if ((x)!=0) std::abort(); } while (false)

struct Restarting {};
inline void esp_restart() { throw Restarting{}; }
