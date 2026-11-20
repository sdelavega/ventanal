// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
#pragma once

#include "sync.h"
#include <ctime>
#include <string>
#include <vector>

namespace lcd {

// One standalone refresh pass (no host): fills `result` the way runRefresh()
// does, publishing frames through cache.publish. Blocking; run it on the
// refresh worker.
void runLocalRefresh(const char currentIds[SCREEN_SLOTS][17], RefreshResult& result, Watchdog& watchdog,
                     const ResidentCache& cache);

std::vector<std::string> wantedArtTargets(time_t now, const char* displayedArtId);

// Persists staged standalone art state on the display loop.
bool saveArtState(const local::ArtState& state);

}  // namespace lcd
