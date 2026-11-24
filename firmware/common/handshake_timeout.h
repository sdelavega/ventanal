// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
#pragma once
#include <cstdint>

// WiFiClientSecure accepts whole seconds. Never round past the budget.
inline uint32_t handshakeTimeoutSeconds(uint32_t remainingMs) {
    return remainingMs >= 20000 ? 20 : remainingMs / 1000;
}
