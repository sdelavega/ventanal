// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
#pragma once

#include "frame.h"
#include "../common/resume/fs_store.h"

namespace lcd {

enum class FrameDecode { Complete, Invalid, Storage, Memory };

inline bool readFrameBytes(fs::File& file, FsStore* store, uint8_t* out, size_t count) {
    return store ? store->readExact(file, out, count) : file.read(out, count) == count;
}

inline bool seekFrame(fs::File& file, FsStore* store, size_t position) {
    return store ? store->seek(file, position) : file.seek(position);
}

bool verifyFrameFile(fs::File& file, const char* id, Watchdog& watchdog);

// File must be at offset zero. Output is scratch until the result is Complete.
FrameDecode decodeFrameFile(FrameEncoding encoding, fs::File& file, FsStore* store,
                            uint8_t* frame, Watchdog& watchdog);
FrameDecode decodeHostJpegFile(fs::File& file, FsStore* store, uint8_t* frame, Watchdog& watchdog);

}  // namespace lcd
