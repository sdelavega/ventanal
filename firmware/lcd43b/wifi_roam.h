// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
#pragma once

namespace lcd {

inline constexpr int WifiRoamThresholdDbm = -67;

inline constexpr bool shouldRoam(int currentDbm, int bestDbm) {
    return currentDbm < WifiRoamThresholdDbm && bestDbm >= currentDbm + 8;
}

}  // namespace lcd
