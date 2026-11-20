// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
#pragma once

#include "partial_store.h"
#include <FS.h>

// Targets are file names under root, with an optional leading slash.
// The .part and .meta suffixes and dot-prefixed names are reserved.
class FsStore : public PartialStore {
public:
    explicit FsStore(fs::FS& fs, const char* root = "/partials", void (*ioError)() = nullptr);

    bool readMeta(const char* target, PartialMeta&) override;
    bool writeMeta(const char* target, const PartialMeta&) override;
    bool append(const char* target, const uint8_t*, size_t) override;
    bool truncate(const char* target, uint32_t size) override;
    int64_t size(const char* target) override;
    // Commit stays on the same filesystem. Replacement on FAT is non-atomic.
    bool commit(const char* target) override;
    void discard(const char* target) override;
    bool sha256(const char* target, uint8_t out[32]) override;

    // Read a verified staging file into the caller's buffer and retire it.
    bool takeCommitted(const char* target, uint8_t* bytes, size_t size);
    fs::File openCommitted(const char* target);
    bool removeCommitted(const char* target);
    void pruneCommitted(const char* prefix, const char* keep);
    void sweep(uint32_t epochNow, bool (*validatorLive)(const PartialMeta&));

private:
    std::string path(const char* target) const;
    bool ensureRoot();
    fs::File openRead(const std::string& path);
    bool remove(const std::string& path);
    bool fail();

    fs::FS& fs_;
    std::string root_;
    void (*ioError_)();
    bool faulted_ = false;
};

bool probePartialStorage(fs::FS& fs);
