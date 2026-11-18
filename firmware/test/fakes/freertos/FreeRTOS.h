// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
#pragma once
using portMUX_TYPE = int;
constexpr int portMUX_INITIALIZER_UNLOCKED=0;
inline void portENTER_CRITICAL(portMUX_TYPE*) {}
inline void portEXIT_CRITICAL(portMUX_TYPE*) {}
constexpr int pdTRUE=1, pdPASS=1;
constexpr unsigned portMAX_DELAY=~0U;
