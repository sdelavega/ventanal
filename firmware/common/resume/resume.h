// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
#pragma once

#include "partial_store.h"
#include <algorithm>
#include <cinttypes>
#include <functional>
#include <new>

inline constexpr uint32_t kResumeChunk = 32768;

struct ResumeSource {
    const char* url;
    const char* validator;
    const char* sha256Hex;
    uint32_t expectedSize;
    bool sendIfRange;
};

struct RangeResponse {
    int status;
    uint32_t start, end, total;
    char validator[72];
};

using ChunkSink = std::function<bool(const uint8_t*, size_t)>;

class RangeTransport {
public:
    // Send Range: bytes=<start>-<end> inclusive. An empty ifRange omits
    // If-Range. Stream 200/206 bodies to sink, stopping if it returns false.
    // Report status -1 for interrupted responses, and total 0 for a 200
    // whose length is unknown until the body has been read to completion.
    virtual RangeResponse get(const ResumeSource&, uint32_t start, uint32_t end,
                              const char* ifRange, ChunkSink&) = 0;
    virtual ~RangeTransport() = default;
};

enum class FetchOutcome { Complete, Progress, NoProgress, Failed };

namespace resume_detail {

inline constexpr uint32_t kMaxBodyBytes = 0x380000;
inline constexpr uint32_t kValidEpoch = 1704067200;

inline bool terminated(const char* text, size_t size) {
    return std::memchr(text, 0, size) != nullptr;
}

// get() supplies response headers after the sink runs. Keep its body separate
// from the durable partial until the status, range, and validator are known.
class ResponseBody {
public:
    ~ResponseBody() {
        while (head_) {
            Chunk* next = head_->next;
            delete head_;
            head_ = next;
        }
    }

    ResponseBody() = default;
    ResponseBody(const ResponseBody&) = delete;
    ResponseBody& operator=(const ResponseBody&) = delete;

    bool collect(const uint8_t* data, size_t count) {
        if (failed || count > kMaxBodyBytes - bytes) {
            failed = true;
            return false;
        }
        while (count) {
            if (!tail_ || tail_->used == kResumeChunk) {
                auto* chunk = new (std::nothrow) Chunk;
                if (!chunk) {
                    failed = true;
                    return false;
                }
                if (tail_) tail_->next = chunk;
                else head_ = chunk;
                tail_ = chunk;
            }
            const size_t copied = std::min(count, size_t(kResumeChunk - tail_->used));
            std::memcpy(tail_->data + tail_->used, data, copied);
            tail_->used += static_cast<uint32_t>(copied);
            bytes += static_cast<uint32_t>(copied);
            data += copied;
            count -= copied;
        }
        return true;
    }

    bool save(PartialStore& store, const char* target, PartialMeta& meta, uint32_t epochNow) const {
        for (const Chunk* chunk = head_; chunk; chunk = chunk->next) {
            if (!store.append(target, chunk->data, chunk->used)) return false;
            meta.have += chunk->used;
            if (epochNow >= kValidEpoch) meta.touchedAt = epochNow;
            if (!store.writeMeta(target, meta)) return false;
        }
        return true;
    }

    uint32_t bytes = 0;
    bool failed = false;

private:
    struct Chunk {
        uint8_t data[kResumeChunk];
        uint32_t used = 0;
        Chunk* next = nullptr;
    };

    Chunk* head_ = nullptr;
    Chunk* tail_ = nullptr;
};

inline unsigned hexDigit(char c) {
    if (c >= '0' && c <= '9') return unsigned(c - '0');
    if (c >= 'a' && c <= 'f') return unsigned(c - 'a' + 10);
    if (c >= 'A' && c <= 'F') return unsigned(c - 'A' + 10);
    return 16;
}

inline bool verifies(PartialStore& store, const char* target, const PartialMeta& meta, const char* hex) {
    if (!meta.total || meta.have != meta.total || store.size(target) != meta.total) return false;
    if (!*hex) return true;
    uint8_t digest[32];
    if (std::strlen(hex) != 2 * sizeof(digest) || !store.sha256(target, digest)) return false;
    for (size_t i = 0; i < sizeof(digest); ++i) {
        if (hexDigit(hex[2 * i]) != (unsigned(digest[i]) >> 4) ||
            hexDigit(hex[2 * i + 1]) != (digest[i] & 0xfu)) return false;
    }
    return true;
}

}  // namespace resume_detail

inline FetchOutcome resumeFetch(RangeTransport& transport, PartialStore& store, const char* target,
                                const ResumeSource& source, uint32_t budgetMs, uint32_t (*nowMs)(),
                                uint32_t epochNow, void (*log)(const char*, ...)) {
    using namespace resume_detail;
    const uint32_t started = nowMs();
    const char* pinned = source.validator ? source.validator : "";
    const char* digest = source.sha256Hex ? source.sha256Hex : "";
    if (!source.url || !*source.url || std::strlen(source.url) >= sizeof(PartialMeta::url) ||
        std::strlen(pinned) >= sizeof(PartialMeta::validator) || source.expectedSize > kMaxBodyBytes) {
        return FetchOutcome::Failed;
    }

    PartialMeta meta{};
    const auto reset = [&] {
        store.discard(target);
        meta = {};
        std::strcpy(meta.url, source.url);
    };
    const auto fail = [&] {
        store.discard(target);
        return FetchOutcome::Failed;
    };
    const auto finish = [&] {
        if (!verifies(store, target, meta, digest) || !store.commit(target)) return fail();
        if (log) log("resume done target=%s bytes=%" PRIu32 " ms=%" PRIu32,
                     target, meta.total, uint32_t(nowMs() - started));
        return FetchOutcome::Complete;
    };

    const int64_t fileSize = store.size(target);
    if (!store.readMeta(target, meta) || fileSize <= 0 || !meta.have || meta.have > meta.total ||
        meta.total > kMaxBodyBytes || !terminated(meta.url, sizeof(meta.url)) ||
        !terminated(meta.validator, sizeof(meta.validator)) || std::strcmp(meta.url, source.url) ||
        (*pinned && std::strcmp(pinned, meta.validator)) || (!*meta.validator && !*digest) ||
        (source.expectedSize && source.expectedSize != meta.total)) {
        reset();
    } else if (fileSize > meta.have) {
        if (!store.truncate(target, meta.have)) return fail();
    } else if (fileSize < meta.have) {
        meta.have = static_cast<uint32_t>(fileSize);
        if (!store.writeMeta(target, meta)) return fail();
    }

    if (meta.have && log) {
        log("resume target=%s from=%" PRIu32 " total=%" PRIu32, target, meta.have, meta.total);
    }

    bool progressed = false;
    bool restarted = false;
    const auto pause = [&] { return progressed ? FetchOutcome::Progress : FetchOutcome::NoProgress; };

    while (uint32_t(nowMs() - started) < budgetMs) {
        const uint32_t start = meta.have;
        const uint32_t total = meta.total ? meta.total : source.expectedSize;
        uint32_t end = start + kResumeChunk - 1;
        if (total) end = start == total ? start : std::min(end, total - 1);

        ResponseBody body;
        ChunkSink sink = [&](const uint8_t* data, size_t count) { return body.collect(data, count); };
        const RangeResponse response = transport.get(source, start, end,
                                                     source.sendIfRange ? meta.validator : "", sink);
        if (body.failed) return FetchOutcome::Failed;
        if (response.status == -1) return pause();
        if (!terminated(response.validator, sizeof(response.validator))) return fail();

        // Fixed sources can use their manifest/offer identity when the HTTP
        // response itself does not carry one. Public sources learn it on GET.
        const char* validator = *response.validator ? response.validator : pinned;
        const bool validatorChanged = start && std::strcmp(meta.validator, validator);
        const bool complete416 = response.status == 416 && start && start == total &&
            (!response.total || response.total == total) &&
            (!*response.validator || !validatorChanged);
        if (complete416) return finish();

        const bool shifted = response.status == 206 && response.start != start;
        const bool changed206 = response.status == 206 && start &&
            (validatorChanged || (response.total && response.total != meta.total));
        if (shifted || changed206 || (response.status == 416 && start)) {
            if (restarted) return fail();
            reset();
            progressed = false;
            restarted = true;
            continue;
        }

        if (response.status != 200 && response.status != 206) return FetchOutcome::Failed;
        if (response.status == 200 && start) {
            reset();
            progressed = false;
        }
        if (*pinned && std::strcmp(pinned, validator)) return fail();

        const bool whole = response.status == 200;
        const uint32_t responseTotal = whole && !response.total ? body.bytes : response.total;
        if (!responseTotal || responseTotal > kMaxBodyBytes ||
            (source.expectedSize && responseTotal != source.expectedSize)) return fail();

        if (!whole && (response.end < start || response.end > end || response.end >= responseTotal)) {
            return fail();
        }
        const uint32_t expectedBytes = whole ? responseTotal : response.end - start + 1;
        if (body.bytes < expectedBytes) return pause();
        if (body.bytes > expectedBytes) return fail();
        if (!whole && response.end + 1 < responseTotal && !*validator && !*digest) return fail();

        meta.total = responseTotal;
        std::strcpy(meta.validator, validator);
        if (!body.save(store, target, meta, epochNow)) return fail();
        if (meta.have == meta.total) return finish();
        progressed = true;
    }
    return pause();
}
