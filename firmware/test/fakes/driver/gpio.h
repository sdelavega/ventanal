// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
#pragma once
#include <cassert>
constexpr int GPIO_NUM_21=21, GPIO_MODE_INPUT_OUTPUT=3;
inline bool pwrHeld=false, deepHold=false, pwrOutput=false;
inline int pwrLevel=1;
inline int gpio_set_level(int pin,int level) { assert(pin==21); pwrLevel=level; return 0; }
inline int gpio_set_direction(int pin,int mode) { assert(pin==21 && mode==GPIO_MODE_INPUT_OUTPUT); pwrOutput=true; return 0; }
inline int gpio_hold_en(int pin) { assert(pin==21 && pwrOutput && pwrLevel==0); pwrHeld=true; return 0; }
inline void gpio_deep_sleep_hold_en() { assert(pwrHeld); deepHold=true; }
inline int gpio_get_level(int pin) { assert(pin==21); return pwrLevel; }
inline int gpio_hold_dis(int pin) { assert(pin==21); pwrHeld=false; return 0; }
