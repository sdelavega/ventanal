// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
#pragma once
#include <cassert>
#include <cstdint>
#define RTC_NOINIT_ATTR
#define RTC_DATA_ATTR
constexpr int ESP_SLEEP_WAKEUP_EXT0=2, ESP_SLEEP_WAKEUP_TIMER=4, ESP_SLEEP_WAKEUP_ALL=0, GPIO_NUM_18=18;
inline int fakeWake=0;
inline uint64_t timerUs=0;
inline bool buttonArmed=false;
struct Sleeping {};
inline int esp_sleep_get_wakeup_cause() { return fakeWake; }
inline void esp_sleep_enable_ext0_wakeup(int,int) { buttonArmed=true; }
inline int lightSleeps=0;
inline int esp_sleep_enable_timer_wakeup(uint64_t us) { timerUs=us; return 0; }
inline int esp_light_sleep_start() { assert(timerUs); ++lightSleeps; return 0; }
inline int esp_sleep_disable_wakeup_source(int source) {
    assert(source==ESP_SLEEP_WAKEUP_ALL || source==ESP_SLEEP_WAKEUP_TIMER);
    timerUs=0;
    if(source==ESP_SLEEP_WAKEUP_ALL) buttonArmed=false;
    return 0;
}
[[noreturn]] inline void esp_deep_sleep_start() { throw Sleeping{}; }
inline unsigned uxTaskGetStackHighWaterMark(void*) { return 4096; }
