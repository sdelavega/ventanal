// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
#include "../lcd43b/wifi_roam.h"

#include <cassert>
#include <cstdio>

int main() {
    using lcd::shouldRoam;
    assert(!shouldRoam(-66, -40));
    assert(!shouldRoam(-67, -40));
    assert(!shouldRoam(-68, -61));
    assert(shouldRoam(-68, -60));
    assert(shouldRoam(-83, -55));
    assert(!shouldRoam(-90, -95));
    puts("lcd43b wifi roam ok");
}
