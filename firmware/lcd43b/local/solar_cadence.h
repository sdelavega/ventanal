// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
#pragma once

#include <cstdint>

namespace lcd::local {

struct SolarCadence {
    static constexpr int64_t Interval = 3 * 3600;
    int64_t fetchedAt = 0;
    int64_t solarDrawnAt = 0;
    uint32_t sunDrawnDay = 0;

    bool swpcDue(int64_t now) const {
        return !fetchedAt || now - fetchedAt >= Interval;
    }

    bool needsFrame(bool hasFrame, uint32_t day, bool todayCommitted) const {
        return fetchedAt && (!hasFrame || fetchedAt > solarDrawnAt ||
                             (todayCommitted && sunDrawnDay != day));
    }
};

enum class SolarSun { None, Previous, Today };

template<class DrawToday, class DrawPrevious>
SolarSun drawSolarSun(DrawToday drawToday, DrawPrevious drawPrevious) {
    if (drawToday()) return SolarSun::Today;
    if (drawPrevious()) return SolarSun::Previous;
    return SolarSun::None;
}

}  // namespace lcd::local
