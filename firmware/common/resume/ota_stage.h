// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
#pragma once

#include "../ota_state.h"
#include "../release_info.h"
#include "fs_store.h"
#include "resume.h"
#include <cstdio>

using OtaStage = FetchOutcome;
using OtaStageLog = void (*)(const char*, ...);

constexpr bool otaInstallFitsWindow(uint32_t started, uint32_t now, uint32_t windowMs,
                                   uint32_t installMs) {
    const uint32_t elapsed = now - started;
    return elapsed < windowMs && installMs <= windowMs - elapsed;
}

inline OtaStage logOtaStage(OtaStage outcome, const char* reason, uint32_t have,
                            uint32_t total, OtaStageLog log) {
    constexpr const char* names[] = {"complete", "progress", "no-progress", "failed"};
    if (log) log("ota stage outcome=%s reason=%s have=%" PRIu32 " total=%" PRIu32,
                 names[static_cast<unsigned>(outcome)], reason, have, total);
    return outcome;
}

template<class FreeBytes, class Evict>
bool makeOtaSpace(uint64_t additional, FreeBytes freeBytes, Evict evict) {
    const uint64_t required = additional + 65536;
    while (freeBytes() < required)
        if (!evict()) return false;
    return true;
}

template<size_t N>
auto spareFrameEvictor(fs::FS& fs, const char* const (&paths)[N]) {
    return [&fs, &paths, next = size_t{0}]() mutable {
        while (next < N) {
            const char* path = paths[next++];
            if (path && fs.remove(path)) return true;
        }
        return false;
    };
}

inline std::string otaStageTarget(const ReleaseInfo& release) {
    char target[sizeof("ota-65535.65535.65535")];
    snprintf(target, sizeof target, "ota-%u.%u.%u",
             unsigned(release.major), unsigned(release.minor), unsigned(release.patch));
    return target;
}

inline OtaFailure verifyOtaFile(FsStore& store, fs::File& file, const ReleaseInfo& release,
                                const HashFeed& feed) {
    if (file.size() != release.size) return OtaFailure::Digest;
    Sha256 hash;
    uint8_t block[kPartialHashBlockBytes];
    for (size_t remaining = release.size; remaining;) {
        if (feed) feed();
        const size_t count = std::min(remaining, sizeof block);
        if (!store.readExact(file, block, count)) return OtaFailure::Install;
        hash.update(block, count);
        remaining -= count;
    }
    return releaseImageMatches(release, file.size(), hash.digest())
        ? OtaFailure::None : OtaFailure::Digest;
}

// The staged image has already verified. Recheck the bytes as they are
// written so a storage fault during this second read cannot activate it.
template<class Read, class Write>
OtaFailure writeStagedImage(const ReleaseInfo& release, Read read, Write write, const HashFeed& feed) {
    constexpr size_t blockSize = 4096;
    std::unique_ptr<uint8_t[]> block(new (std::nothrow) uint8_t[blockSize]);
    if (!block) return OtaFailure::Install;
    Sha256 hash;
    size_t written = 0;
    while (written < release.size) {
        if (feed) feed();
        const size_t count = std::min(blockSize, size_t(release.size) - written);
        if (read(block.get(), count) != count || !write(block.get(), count))
            return OtaFailure::Install;
        hash.update(block.get(), count);
        written += count;
    }
    return releaseImageMatches(release, written, hash.digest())
        ? OtaFailure::None : OtaFailure::Digest;
}

// Callers pass metadata authenticated by their existing release-signature
// check. The same file handle is verified, rewound and handed to the writer.
// A writer's short read aborts the install without forcing a store switch.
template<class Write>
OtaFailure installStaged(FsStore& store, const ReleaseInfo& release, const HashFeed& feed,
                         Write write) {
    const auto target = otaStageTarget(release);
    auto file = store.openCommitted(target.c_str());
    if (!file) return OtaFailure::Digest;
    auto failure = verifyOtaFile(store, file, release, feed);
    if (failure == OtaFailure::None)
        failure = file.seek(0) ? write(file) : OtaFailure::Install;
    file.close();
    if (failure == OtaFailure::None || failure == OtaFailure::Digest)
        store.removeCommitted(target.c_str());
    if (failure == OtaFailure::Digest) store.discard(target.c_str());
    return failure;
}

template<class FreeBytes, class Evict>
OtaStage stageUpdate(RangeTransport& transport, const ReleaseInfo& release, uint32_t budgetMs,
                     FsStore& store, const char* url, bool sendIfRange, uint32_t (*nowMs)(),
                     uint32_t epochNow, FreeBytes freeBytes, Evict evict, OtaStageLog log = nullptr,
                     const HashFeed& feed = {}, const char** resultReason = nullptr) {
    const auto targetName = otaStageTarget(release);
    const char* target = targetName.c_str();
    const auto report = [&](OtaStage result, const char* reason, uint32_t have = 0) {
        if (resultReason) *resultReason = reason;
        return logOtaStage(result, reason, have, release.size, log);
    };
    if (!url || !*url || strlen(url) >= sizeof(PartialMeta::url))
        return report(OtaStage::Failed, "url");
    if (!release.size || release.size > resume_detail::kMaxBodyBytes)
        return report(OtaStage::Failed, "size");
    if (!budgetMs) return report(OtaStage::NoProgress, "budget");

    const uint32_t started = nowMs();
    store.retainOtaTarget(target);
    auto file = store.openCommitted(target);
    if (file) {
        const bool sizeMatches = file.size() == release.size;
        file.close();
        if (sizeMatches) return report(OtaStage::Complete, "ok", release.size);
        store.removeCommitted(target);
    }

    constexpr char hex[] = "0123456789abcdef";
    char digest[65]{};
    for (size_t i = 0; i < sizeof release.sha256; ++i) {
        digest[2 * i] = hex[release.sha256[i] >> 4];
        digest[2 * i + 1] = hex[release.sha256[i] & 15];
    }
    char validator[sizeof(PartialMeta::validator)];
    snprintf(validator, sizeof validator, "%s:%s:%" PRIu32, target + 4, digest, release.size);
    PartialMeta meta{};
    const auto stored = store.size(target);
    uint32_t have = 0;
    if (stored > 0 && store.readMeta(target, meta) && meta.total == release.size &&
        !strcmp(meta.validator, validator) && !strcmp(meta.url, url))
        have = std::min<uint32_t>(stored, meta.have);
    else
        store.discard(target);

    if (!makeOtaSpace(release.size - have, freeBytes, evict)) {
        store.discard(target);
        return report(OtaStage::Failed, "space");
    }

    const ResumeSource source{url, validator, digest, release.size, sendIfRange, true};
    const char* reason = nullptr;
    const uint32_t elapsed = nowMs() - started;
    const auto outcome = resumeFetch(transport, store, target, source,
                                     elapsed < budgetMs ? budgetMs - elapsed : 0,
                                     nowMs, epochNow, log, &reason, feed);
    if (outcome == OtaStage::Complete) return report(outcome, "ok", release.size);
    if (outcome == OtaStage::Failed) store.discard(target);
    have = store.readMeta(target, meta) ? meta.have : 0;
    return report(outcome, reason ? reason : outcome == OtaStage::Failed ? "http" : "pass-ended", have);
}
