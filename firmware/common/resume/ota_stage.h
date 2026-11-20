// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
#pragma once

#include "../release_info.h"
#include "fs_store.h"
#include "resume.h"
#include <cstdio>

using OtaStage = FetchOutcome;
using OtaStageLog = void (*)(const char*, ...);

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

template<class Feed>
bool verifyOtaFile(FsStore& store, fs::File& file, const ReleaseInfo& release, Feed feed) {
    if (!file || file.size() != release.size) return false;
    Sha256 hash;
    uint8_t block[1024];
    for (size_t remaining = release.size; remaining;) {
        feed();
        const size_t count = std::min(remaining, sizeof block);
        if (!store.readExact(file, block, count)) return false;
        hash.update(block, count);
        remaining -= count;
    }
    return releaseImageMatches(release, file.size(), hash.digest()) && file.seek(0);
}

// Callers pass metadata authenticated by their existing release-signature
// check. The same file handle is verified, rewound and handed to the writer.
template<class Feed, class Write>
bool installStaged(FsStore& store, const ReleaseInfo& release, Feed feed, Write write) {
    const auto target = otaStageTarget(release);
    auto file = store.openCommitted(target.c_str());
    const bool verified = verifyOtaFile(store, file, release, feed);
    const bool installed = verified && write(file);
    file.close();
    store.removeCommitted(target.c_str());
    if (!verified) store.discard(target.c_str());
    return installed;
}

template<class FreeBytes, class Evict, class Feed = void (*)()>
OtaStage stageUpdate(RangeTransport& transport, const ReleaseInfo& release, uint32_t budgetMs,
                     FsStore& store, const char* url, bool sendIfRange, uint32_t (*nowMs)(),
                     uint32_t epochNow, FreeBytes freeBytes, Evict evict, OtaStageLog log = nullptr,
                     Feed feed = +[] {}, const char** resultReason = nullptr) {
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
        const bool verified = verifyOtaFile(store, file, release, feed);
        file.close();
        if (verified) return report(OtaStage::Complete, "ok", release.size);
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
                                     nowMs, epochNow, log, &reason);
    if (outcome == OtaStage::Complete) return report(outcome, "ok", release.size);
    if (outcome == OtaStage::Failed) store.discard(target);
    have = store.readMeta(target, meta) ? meta.have : 0;
    return report(outcome, reason ? reason : outcome == OtaStage::Failed ? "http" : "pass-ended", have);
}
