// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
//
// Board-only media helpers for standalone frames, using code in the
// ESP32-S3 ROM (no flash cost): TJpgDec for baseline JPEGs and miniz's
// compressor for zlib cache bodies.
#pragma once

#include <FS.h>

#include "deferred_save.h"
#include "local/art_compose.h"
#include "local/render_solar.h"
#include "sync.h"

namespace lcd {

// zlib-compresses a decoded VNR1 frame (FRAME_BYTES) into a PSRAM body, the
// same wire form a host sends; false when memory runs out.
bool deflateFrame(const uint8_t* frame, FrameBody& body, Watchdog& watchdog);

// Composes an art frame (pixels of `f`) from a baseline JPEG with the
// server's layout and credit; false for a progressive or damaged JPEG.
bool composeArtJpeg(const uint8_t* jpeg, size_t length, local::Frame f, const local::ArtCredit& credit,
                    Watchdog& watchdog, uint32_t budgetMs = UINT32_MAX);

// Draws the SDO image (a baseline JPEG) into local::SUN_RECT of `f`, cropped
// like the server's crop_sun() and decoded at the smallest scale that still
// covers the disc; false for an unreadable JPEG.
bool drawSunJpeg(fs::File& jpeg, local::Frame f, Watchdog& watchdog);

// Art is cached as its source, not its pixels (a photo barely compresses and
// PSRAM has no room for a second 768 KB copy): a "VNA1" body holds the credit
// and the JPEG as downloaded, and the cache loader composes the frame again.
//   bytes 0..3  "VNA1"
//   bytes 4..7  credit length (little endian): title, artist, date, museum,
//               each NUL-terminated
//   then the credit, then the JPEG.
inline constexpr uint8_t ArtBodyMagic[4] = {'V', 'N', 'A', '1'};
size_t artBodyHeaderBytes(const local::ArtCredit& credit);
void writeArtBodyHeader(uint8_t* body, const local::ArtCredit& credit);
bool isArtBody(const uint8_t* body, size_t length);
// Composes the frame (header and pixels, FRAME_BYTES) a VNA1 body describes.
bool composeArtBody(const uint8_t* body, size_t length, uint8_t* frame, Watchdog& watchdog);

}  // namespace lcd
