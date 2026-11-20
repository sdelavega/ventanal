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
// zlib compressBound for one complete VNR1 frame, also the JPEG wire cap.
constexpr size_t FRAME_WIRE_MAX = FRAME_BYTES + (FRAME_BYTES >> 12)
    + (FRAME_BYTES >> 14) + (FRAME_BYTES >> 25) + 13;

enum class FrameEncoding { Raw, Zlib, Jpeg };

inline bool validWireSize(FrameEncoding encoding, size_t size) {
    return encoding == FrameEncoding::Raw ? size == FRAME_BYTES
        : size >= 8 && size <= FRAME_WIRE_MAX;
}

// Cached JPEGs are stored as the original wire bytes, like zlib and VNA1.
inline bool isJpegFrame(const uint8_t* data, size_t size) {
    return data && size >= 2 && data[0] == 0xff && data[1] == 0xd8;
}

inline bool validJpegEnvelope(const uint8_t* data, size_t size) {
    return validWireSize(FrameEncoding::Jpeg, size) && isJpegFrame(data, size)
        && data[size - 2] == 0xff && data[size - 1] == 0xd9;
}

// TJpgDec produces packed RGB888 MCU blocks. Write each directly into the
// VNR1 staging pixels, rejecting any rectangle extending beyond the panel.
inline bool writeJpegBlock(uint8_t* frame, const uint8_t* rgb, unsigned x, unsigned y,
                           unsigned w, unsigned h) {
    if (x >= WIDTH || y >= HEIGHT || !w || !h || w > WIDTH - x || h > HEIGHT - y)
        return false;
    for (unsigned row = 0; row < h; ++row) {
        uint8_t* dst = frame + HEADER_BYTES + (size_t(y + row) * WIDTH + x) * 2;
        for (unsigned col = 0; col < w; ++col, rgb += 3, dst += 2) {
            const uint16_t pixel = (uint16_t(rgb[0] & 0xf8) << 8)
                | (uint16_t(rgb[1] & 0xfc) << 3) | (rgb[2] >> 3);
            dst[0] = uint8_t(pixel);
            dst[1] = uint8_t(pixel >> 8);
        }
    }
    return true;
}

class Watchdog;

// Decode a full baseline JPEG into FRAME_BYTES of scratch, without scaling.
bool decodeHostJpeg(const uint8_t* data, size_t size, uint8_t* frame, Watchdog& watchdog);

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
