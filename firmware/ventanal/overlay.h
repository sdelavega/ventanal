// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
#pragma once
#include <Inkplate.h>
#include "logic.h"
void drawReadout(Inkplate& d, const ReadoutRect& r, int pct, bool offline,
                 bool low, time_t drawnAt, bool clock24);
void drawWaiting(Inkplate& d, const char* url);
void drawChargeMe(Inkplate& d);
