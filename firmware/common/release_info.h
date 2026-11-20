// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

struct ReleaseInfo {
    char board[24];
    uint16_t major, minor, patch;
    uint32_t size;
    uint8_t sha256[32];
    uint16_t minMajor, minMinor, minPatch;
};

// Call with the measured size and final SHA-256 of the streamed image.
inline bool releaseImageMatches(const ReleaseInfo& info, size_t size, const uint8_t sha256[32]) {
    return size == info.size && memcmp(info.sha256, sha256, sizeof(info.sha256)) == 0;
}
