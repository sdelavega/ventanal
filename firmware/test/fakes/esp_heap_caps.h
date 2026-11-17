// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
#pragma once
#include <cstdlib>
constexpr unsigned MALLOC_CAP_SPIRAM = 1, MALLOC_CAP_8BIT = 2, MALLOC_CAP_INTERNAL = 4;
inline bool failAllocation = false;
inline unsigned liveAllocations = 0;
inline void* heap_caps_malloc(size_t n, unsigned caps) {
    if (failAllocation || caps != (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)) return nullptr;
    void* p = std::malloc(n);
    if (p) ++liveAllocations;
    return p;
}
inline void heap_caps_free(void* p) { if (p) --liveAllocations; std::free(p); }
