// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
#pragma once

// Serial logging over the native USB-Serial/JTAG port, shared by the
// sketch and the sync engine. Every line is one bounded write() (never a
// per-character trickle), and a flush runs only while a host actually has
// the CDC port open, so a missing or stalled host can never block the
// caller. Lines printed before a host connects stay in the enlarged TX
// ring buffer (oldest dropped) and surface as soon as one does.

#include <Arduino.h>

#include <cstdarg>
#include <cstddef>
#include <cstdio>

namespace lcd {

inline void log(const char* format, ...) {
    char line[192];
    va_list args;
    va_start(args, format);
    const int length = vsnprintf(line, sizeof line, format, args);
    va_end(args);
    if (length <= 0) return;
    const size_t used = size_t(length) < sizeof line - 1 ? size_t(length)
                                                        : sizeof line - 1;
    Serial.write(reinterpret_cast<const uint8_t*>(line), used);
    if (Serial) Serial.flush();
}

}  // namespace lcd
