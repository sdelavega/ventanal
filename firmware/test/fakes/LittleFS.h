// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
#pragma once

#include "FS.h"

struct FakeFS : fs::FS {
    size_t capacity = 0x1BD0000;
    size_t totalBytes() const { return capacity; }
    size_t usedBytes() const {
        size_t bytes = 0;
        for (const auto& file : files) bytes += file.second.size();
        return bytes;
    }
    bool failBegin = false;
    bool failFormat = false;
    unsigned formatAttempts = 0;
    bool begin(bool formatOnFail, const char*, int, const char*) {
        if (!failBegin) return true;
        if (!formatOnFail) return false;
        ++formatAttempts;
        if (failFormat) return false;
        files.clear();
        directories = {"/"};
        failBegin = false;
        return true;
    }
    void end() {}
};
inline FakeFS LittleFS;
inline auto& files = LittleFS.files;
