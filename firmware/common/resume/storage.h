// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
#pragma once

#include "partial_store.h"

namespace fs { class FS; }

// The board registers its filesystem after mounting and probing it.
void setPartialSdFilesystem(fs::FS& fs);
PartialStore& partialStore();
void selectPartialStore(bool sdMounted);
void sweepPartials(uint32_t epochNow, bool (*validatorLive)(const PartialMeta&));
