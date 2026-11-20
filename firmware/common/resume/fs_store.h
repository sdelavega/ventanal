// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
#pragma once

#include "partial_store.h"
#include <FS.h>
#include <mutex>
#include <set>

// Targets are file names under root, with an optional leading slash.
// The .part, .meta, .fail and .temp suffixes and dot-prefixed names are reserved.
class FsStore : public PartialStore {
public:
    explicit FsStore(fs::FS& fs, const char* root = "/partials", void (*ioError)() = nullptr);

    bool readMeta(const char* target, PartialMeta&) override;
    bool writeMeta(const char* target, const PartialMeta&) override;
    bool append(const char* target, const uint8_t*, size_t) override;
    bool keepAfterWriteFailure(const char* target, uint32_t have) override;
    bool truncate(const char* target, uint32_t size) override;
    int64_t size(const char* target) override;
    // Commit stays on the same filesystem. Replacement on FAT is non-atomic.
    bool commit(const char* target) override;
    void discard(const char* target) override;
    void resetPartial(const char* target) override;
    bool sha256(const char* target, uint8_t out[32], const HashFeed& feed = {}) override;

    // Read a verified staging file into the caller's buffer and retire it.
    bool takeCommitted(const char* target, uint8_t* bytes, size_t size);
    bool readCommitted(const char* target, uint8_t* bytes, size_t size);
    fs::File openCommitted(const char* target);
    // Promote a verified stage on this filesystem with a single rename.
    bool renameCommitted(const char* target, const char* destination);
    bool readExact(fs::File& file, uint8_t* bytes, size_t count);
    void retainOtaTarget(const char* target);
    bool removeCommitted(const char* target);
    // Return the newest committed day before today. Cleanup is done by the LCD sweep.
    std::string previousCommitted(const char* prefix, uint32_t today);
    using ValidatorLive = std::function<bool(const char*, const PartialMeta&)>;
    using TargetFilter = std::function<bool(const char*)>;
    void sweep(uint32_t epochNow, bool (*validatorLive)(const PartialMeta&));
    void sweep(uint32_t epochNow, const ValidatorLive& validatorLive, const TargetFilter& managed);
    struct CommittedEntry {
        std::string target;
        uint32_t modified;
    };
    std::vector<CommittedEntry> committedEntries();
    void setWriteHook(void (*hook)(bool)) { writeHook_ = hook; }

private:
    bool readFile(const std::string& path, uint8_t* bytes, size_t size);
    std::string path(const char* target) const;
    bool ensureRoot();
    fs::File openRead(const std::string& path);
    bool remove(const std::string& path);
    bool fail();
    bool rename(const std::string& from, const std::string& to);
    bool write(const std::string& path, const uint8_t* bytes, size_t size, bool append);
    void discardFiles(const std::string& base);
    void (*writeHook_)(bool) = nullptr;
    void clearWriteFailure(const std::string& base);

    fs::FS& fs_;
    std::string root_;
    void (*ioError_)();
    bool faulted_ = false;
    // Used when flash cannot even rename the sidecar to record a strike.
    std::mutex checkpointMutex_;
    std::set<std::string> failedCheckpoints_;
    std::set<std::string> emptyStrikes_;
};

bool probePartialStorage(fs::FS& fs);
