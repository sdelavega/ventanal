// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
#pragma once

// VNR1 raw frame format for the 800x480 full-colour LCD profile:
//   bytes 0..3   magic "VNR1"
//   bytes 4..5   width, little-endian (800)
//   bytes 6..7   height, little-endian (480)
//   bytes 8..15  reserved, zero
//   bytes 16..   RGB565 pixels, little-endian, row-major
// Total: 16 + 800 * 480 * 2 = 768,016 bytes.

#include <cstddef>
#include <cstdint>

#include "sha256.h"

namespace lcd {

constexpr uint16_t WIDTH = 800;
constexpr uint16_t HEIGHT = 480;
constexpr size_t HEADER_BYTES = 16;
constexpr size_t PIXEL_BYTES = size_t(WIDTH) * HEIGHT * 2;
constexpr size_t FRAME_BYTES = HEADER_BYTES + PIXEL_BYTES;

// True when the 16 bytes are a VNR1 header for this panel.
inline bool headerValid(const uint8_t* header) {
    if (header[0] != 'V' || header[1] != 'N' || header[2] != 'R' || header[3] != '1')
        return false;
    if (header[4] != 0x20 || header[5] != 0x03) return false;  // 800 LE
    if (header[6] != 0xE0 || header[7] != 0x01) return false;  // 480 LE
    for (size_t i = 8; i < HEADER_BYTES; ++i)
        if (header[i] != 0) return false;
    return true;
}

// True when a complete frame matches the expected length, header and hash.
inline bool frameVerified(const uint8_t* data, size_t length, const char* sha256Hex) {
    if (length != FRAME_BYTES || !headerValid(data)) return false;
    if (!sha256Hex) return false;
    Sha256 hash;
    hash.update(data, length);
    const uint8_t* digest = hash.digest();
    static const char DIGITS[] = "0123456789abcdef";
    for (int i = 0; i < 32; ++i) {
        if (sha256Hex[2 * i] != DIGITS[digest[i] >> 4]) return false;
        if (sha256Hex[2 * i + 1] != DIGITS[digest[i] & 0x0F]) return false;
    }
    return sha256Hex[64] == '\0';
}

// LCD frame ids are the first eight bytes of the content hash. Checking
// them at boot also catches a reset between the rename and the NVS update.
inline bool cachedFrameValid(const uint8_t* data, const char* id) {
    if (!headerValid(data)) return false;
    Sha256 hash;
    hash.update(data, FRAME_BYTES);
    const uint8_t* digest = hash.digest();
    static const char DIGITS[] = "0123456789abcdef";
    for (int i = 0; i < 8; ++i) {
        if (id[2 * i] != DIGITS[digest[i] >> 4] ||
            id[2 * i + 1] != DIGITS[digest[i] & 0x0F]) return false;
    }
    return id[16] == '\0';
}

}  // namespace lcd
