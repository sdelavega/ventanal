// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
#pragma once
#include <cstdlib>
#include <map>
#include <cstdint>
constexpr unsigned MALLOC_CAP_SPIRAM = 1, MALLOC_CAP_8BIT = 2, MALLOC_CAP_INTERNAL = 4;
inline bool failAllocation = false;
inline unsigned liveAllocations = 0;
inline size_t psramLargest = SIZE_MAX;
inline size_t psramCapacity = SIZE_MAX;
inline size_t psramInUse = 0;
inline size_t psramPeak = 0;
inline std::map<void*, size_t> psramBlocks;
inline void* heap_caps_malloc(size_t n, unsigned caps) {
    if (failAllocation || caps != (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)) return nullptr;
    void* p = std::malloc(n);
    if (p) ++liveAllocations;
    return p;
}
inline void heap_caps_free(void* p) {
    const auto block = psramBlocks.find(p);
    if (block != psramBlocks.end()) {
        psramInUse -= block->second;
        psramBlocks.erase(block);
    }
    if (p) --liveAllocations;
    std::free(p);
}
// PSRAM allocation as the lcd43b sync engine uses it (plain heap here).
inline void* ps_malloc(size_t n) {
    if (failAllocation || n > psramLargest || n > psramCapacity - psramInUse) return nullptr;
    void* p = std::malloc(n);
    if (p) {
        ++liveAllocations;
        if (psramCapacity != SIZE_MAX || psramLargest != SIZE_MAX) {
            psramBlocks[p] = n;
            psramInUse += n;
            if (psramPeak < psramInUse) psramPeak = psramInUse;
        }
    }
    return p;
}
