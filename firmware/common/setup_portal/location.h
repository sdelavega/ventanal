// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
#pragma once

#include <cmath>

namespace portal {

// Where a ZIP resolved to at setup, and the POSIX timezone stored for it.
struct LocationResult {
    enum Why { Ok, ZipUnknown, Network, TzUnknown, Skipped };
    bool ok = false;
    float latitude = NAN;
    float longitude = NAN;
    char place[48]{};    // "City, ST"
    char posixTz[64]{};  // e.g. "EST5EDT,M3.2.0,M11.1.0"
    Why why = Network;
};

}  // namespace portal
