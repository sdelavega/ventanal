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
// zlib compressBound for one complete VNR1 frame.
constexpr size_t FRAME_WIRE_MAX = FRAME_BYTES + (FRAME_BYTES >> 12)
    + (FRAME_BYTES >> 14) + (FRAME_BYTES >> 25) + 13;

enum class FrameEncoding { Raw, Zlib };

inline bool validWireSize(FrameEncoding encoding, size_t size) {
    return encoding == FrameEncoding::Raw ? size == FRAME_BYTES
        : size >= 8 && size <= FRAME_WIRE_MAX;
}

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

inline bool lowerHex(const char* text, size_t length) {
    for (size_t i = 0; i < length; ++i) {
        const char c = text[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }
    return true;
}

// Check the digest of the exact bytes served, or its 16-character cache id.
inline bool frameHashMatches(const uint8_t* data, size_t length,
                             const char* hex, size_t digits) {
    Sha256 hash;
    hash.update(data, length);
    const uint8_t* digest = hash.digest();
    static const char digitsLower[] = "0123456789abcdef";
    for (size_t i = 0; i < digits; ++i) {
        const uint8_t byte = digest[i / 2];
        if (hex[i] != digitsLower[i % 2 ? byte & 0x0F : byte >> 4]) return false;
    }
    return hex[digits] == '\0';
}

inline bool frameVerified(const uint8_t* data, size_t length, const char* sha256Hex) {
    return length == FRAME_BYTES && headerValid(data) && sha256Hex
        && frameHashMatches(data, length, sha256Hex, 64);
}

inline bool cachedFrameValid(const uint8_t* data, const char* id) {
    return headerValid(data) && frameHashMatches(data, FRAME_BYTES, id, 16);
}

}  // namespace lcd
