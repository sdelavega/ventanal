// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
#pragma once

#include <cstdint>

// What the FCC lookup said about the configured amateur call sign. Stored as
// a byte in each board's installation record; shared by both boards.
enum class CallState : uint8_t {
    None,        // no call sign configured, or not looked up yet
    Verified,    // the FCC lists an active amateur licence
    Unverified,  // the lookup failed; the call sign is trusted (owner decision)
    Rejected,    // the FCC answered and has no such active licence
};
inline constexpr uint8_t callStateCount = 4;
