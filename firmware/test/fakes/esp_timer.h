// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
#pragma once
struct esp_timer_create_args_t { void (*callback)(void*); const char* name; };
using esp_timer_handle_t = int*;
inline void (*timerCallback)(void*)=nullptr;
inline bool timerRunning=false;
inline int esp_timer_create(const esp_timer_create_args_t* args,esp_timer_handle_t* h) {
    static int timer; *h=&timer; timerCallback=args->callback; return 0;
}
inline int esp_timer_start_periodic(esp_timer_handle_t,unsigned) { timerRunning=true;return 0; }
inline int esp_timer_stop(esp_timer_handle_t) { timerRunning=false;return 0; }
inline int esp_timer_delete(esp_timer_handle_t) { timerCallback=nullptr;return 0; }
