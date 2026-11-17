// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
#pragma once
using esp_err_t=int;
constexpr int ESP_ERR_INVALID_STATE=1;
struct esp_task_wdt_config_t { unsigned timeout_ms, idle_core_mask; bool trigger_panic; };
using esp_task_wdt_user_handle_t=void*;
inline unsigned watchdogTimeout=0;
inline bool watchdogArmed=false;
inline int esp_task_wdt_init(const esp_task_wdt_config_t* c) {
    assert(c->idle_core_mask==0 && c->trigger_panic); watchdogTimeout=c->timeout_ms; return 0;
}
inline int esp_task_wdt_reconfigure(const esp_task_wdt_config_t* c) { return esp_task_wdt_init(c); }
inline int esp_task_wdt_add_user(const char*,esp_task_wdt_user_handle_t* h) {
    watchdogArmed=true; *h=nullptr; return 0;
}
inline unsigned watchdogFeeds=0;
inline int esp_task_wdt_reset_user(esp_task_wdt_user_handle_t) { ++watchdogFeeds; return 0; }
