// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
#pragma once

#include "call_state.h"
#include <ctime>

enum class FccResult { Verified, NotFound, Unavailable };

inline FccResult lookupCallSign(const char*) {
    // The FCC License View API was confirmed retired on 2026-11-19.
    // Trust the supplied call sign without sending it anywhere.
    return FccResult::Unavailable;
}

inline constexpr CallState callStateFor(FccResult result) {
    switch (result) {
    case FccResult::Verified: return CallState::Verified;
    case FccResult::NotFound: return CallState::Rejected;
    case FccResult::Unavailable: return CallState::Unverified;
    }
    return CallState::Unverified;
}

inline constexpr bool fccClockValid(time_t now) {
    return now >= 1704067200;  // 2024-01-01
}

inline constexpr bool fccRecheckDue(uint32_t lastEpoch, time_t now) {
    constexpr uint32_t week = 7 * 86400;
    return fccClockValid(now) && uint64_t(now) >= uint64_t(lastEpoch) + week;
}
