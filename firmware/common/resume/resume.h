// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
#pragma once

#include "partial_store.h"
#include <algorithm>
#include <cassert>
#include <cinttypes>
#include <functional>
#include <memory>
#include <new>
#include <strings.h>

inline constexpr uint32_t kResumeChunk = 32768;

struct ResumeSource {
    const char* url;
    const char* validator;
    const char* sha256Hex;
    uint32_t expectedSize;
    bool sendIfRange;
    bool separateWireValidator = false;
};

struct RangeResponse {
    int status;
    uint32_t start, end, total;
    char validator[72];
};

using ChunkSink = std::function<bool(const uint8_t*, size_t)>;
using ResponseHeaders = std::function<bool(const RangeResponse&)>;

class RangeTransport {
public:
    // Send Range: bytes=<start>-<end> inclusive. An empty ifRange omits
    // If-Range. Call headers once when the response headers are available,
    // then send only 200/206 body bytes to sink. Use total 0 for a 200
    // with unknown length and an empty validator when none is usable.
    // Stop and return false on interruption or callback rejection. Return
    // true only after receiving the complete HTTP response.
    virtual bool get(const ResumeSource&, uint32_t start, uint32_t end,
                     const char* ifRange, const ResponseHeaders& headers,
                     const ChunkSink& sink) = 0;
    virtual const char* failureReason() const { return nullptr; }
    virtual ~RangeTransport() = default;
};

enum class FetchOutcome { Complete, Progress, NoProgress, Failed };

namespace resume_detail {

inline constexpr uint32_t kMaxBodyBytes = 0x380000;
inline constexpr uint32_t kValidEpoch = 1704067200;

inline bool readRangeNumber(const char*& cursor, uint32_t& value, char delimiter) {
    if (*cursor < '0' || *cursor > '9') return false;
    value = 0;
    do {
        value = value * 10 + uint32_t(*cursor++ - '0');
        if (value > resume_detail::kMaxBodyBytes) return false;
    } while (*cursor >= '0' && *cursor <= '9');
    if (*cursor != delimiter) return false;
    cursor += delimiter != '\0';
    return true;
}

inline bool parseContentRange(const char* cursor, RangeResponse& response) {
    if (strncasecmp(cursor, "bytes ", 6) != 0) return false;
    cursor += 6;
    if (response.status == 416) {
        if (strncmp(cursor, "*/", 2) != 0) return false;
        cursor += 2;
        return readRangeNumber(cursor, response.total, '\0');
    }
    return readRangeNumber(cursor, response.start, '-') && readRangeNumber(cursor, response.end, '/') &&
        readRangeNumber(cursor, response.total, '\0') &&
        response.start <= response.end && response.end < response.total;
}

inline bool terminated(const char* text, size_t size) {
    return std::memchr(text, 0, size) != nullptr;
}

inline unsigned hexDigit(char c) {
    if (c >= '0' && c <= '9') return unsigned(c - '0');
    if (c >= 'a' && c <= 'f') return unsigned(c - 'a' + 10);
    if (c >= 'A' && c <= 'F') return unsigned(c - 'A' + 10);
    return 16;
}

inline const char* verificationFailure(PartialStore& store, const char* target,
                                       const PartialMeta& meta, const char* hex,
                                       const HashFeed& feed) {
    if (!meta.total || meta.have != meta.total) return "size";
    const auto storedSize = store.size(target);
    if (storedSize < 0) return "storage";
    if (storedSize != meta.total) return "size";
    if (!*hex) return nullptr;
    uint8_t digest[32];
    if (std::strlen(hex) != 2 * sizeof(digest)) return "hash";
    if (!store.sha256(target, digest, feed)) return "storage";
    for (size_t i = 0; i < sizeof(digest); ++i) {
        if (hexDigit(hex[2 * i]) != (unsigned(digest[i]) >> 4) ||
            hexDigit(hex[2 * i + 1]) != (digest[i] & 0xfu)) return "hash";
    }
    return nullptr;
}

}  // namespace resume_detail

// A non-null chunkBuffer must provide at least kResumeChunk writable bytes.
inline FetchOutcome resumeFetch(RangeTransport& transport, PartialStore& store, const char* target,
                                const ResumeSource& source, uint32_t budgetMs, uint32_t (*nowMs)(),
                                uint32_t epochNow, void (*log)(const char*, ...),
                                const char** failureReason = nullptr, const HashFeed& feed = {},
                                uint8_t* chunkBuffer = nullptr, size_t chunkBufferSize = 0) {
    using namespace resume_detail;
    assert(!chunkBuffer || chunkBufferSize >= kResumeChunk);
    if (chunkBuffer && chunkBufferSize < kResumeChunk) {
        if (failureReason) *failureReason = "memory";
        return FetchOutcome::Failed;
    }
    const uint32_t started = nowMs();
    const char* pinned = source.validator ? source.validator : "";
    const bool separate = source.separateWireValidator;
    const char* digest = source.sha256Hex ? source.sha256Hex : "";
    if (failureReason) *failureReason = nullptr;
    if (!source.url || !*source.url || std::strlen(source.url) >= sizeof(PartialMeta::url) ||
        std::strlen(pinned) >= sizeof(PartialMeta::validator) || source.expectedSize > kMaxBodyBytes) {
        if (failureReason) *failureReason = "parse";
        return FetchOutcome::Failed;
    }

    PartialMeta meta{};
    const auto reset = [&] {
        store.discard(target);
        meta = {};
        std::strcpy(meta.url, source.url);
    };
    const auto fail = [&](const char* reason) {
        if (failureReason) *failureReason = reason;
        store.discard(target);
        return FetchOutcome::Failed;
    };
    const auto finish = [&] {
        if (const char* reason = verificationFailure(store, target, meta, digest, feed))
            return fail(reason);
        if (!store.commit(target)) return fail("storage");
        if (log) log("resume done target=%s bytes=%" PRIu32 " ms=%" PRIu32,
                     target, meta.total, uint32_t(nowMs() - started));
        return FetchOutcome::Complete;
    };

    const int64_t fileSize = store.size(target);
    if (!store.readMeta(target, meta) || fileSize <= 0 || !meta.have ||
        meta.have > kMaxBodyBytes || (meta.total && meta.have > meta.total) ||
        meta.total > kMaxBodyBytes || !terminated(meta.url, sizeof(meta.url)) ||
        !terminated(meta.validator, sizeof(meta.validator)) || std::strcmp(meta.url, source.url) ||
        !terminated(meta.wireValidator, sizeof(meta.wireValidator)) ||
        (*pinned && std::strcmp(pinned, meta.validator)) || (!*meta.validator && !*digest) ||
        (source.expectedSize && source.expectedSize != meta.total)) {
        reset();
    } else if (fileSize > meta.have) {
        if (!store.truncate(target, meta.have)) return fail("storage");
    } else if (fileSize < meta.have) {
        meta.have = static_cast<uint32_t>(fileSize);
        if (!store.writeMeta(target, meta)) return fail("storage");
    }

    if (meta.have && log) {
        log("resume target=%s from=%" PRIu32 " total=%" PRIu32, target, meta.have, meta.total);
    }

    // Reuse the same buffer for each response. Whole responses flush full
    // chunks during delivery. A range stays buffered until framing succeeds.
    std::unique_ptr<uint8_t[]> buffer;
    bool progressed = false;
    bool restarted = false;
    const auto pause = [&] { return progressed ? FetchOutcome::Progress : FetchOutcome::NoProgress; };

    while (uint32_t(nowMs() - started) < budgetMs) {
        const uint32_t start = meta.have;
        const uint32_t total = meta.total ? meta.total : source.expectedSize;
        uint32_t end = start + kResumeChunk - 1;
        if (total) end = start == total ? start : std::min(end, total - 1);

        RangeResponse response{};
        uint32_t buffered = 0;
        const char* rejection = nullptr;
        bool allocationFailed = false;
        const auto saveChunk = [&] {
            if (!buffered) return true;
            if (!store.append(target, chunkBuffer, buffered)) return false;
            meta.have += buffered;
            if (epochNow >= kValidEpoch) meta.touchedAt = epochNow;
            if (!store.writeMeta(target, meta)) return false;
            buffered = 0;
            progressed = true;
            return true;
        };
        const ResponseHeaders headers = [&](const RangeResponse& incoming) {
            response = incoming;
            if (response.status != 200) return true;
            reset();
            progressed = false;
            if (!terminated(response.validator, sizeof(response.validator))) {
                rejection = "parse";
                return false;
            }
            if (!separate && *pinned && *response.validator && std::strcmp(pinned, response.validator)) {
                rejection = "validator";
                return false;
            }
            if (response.total > kMaxBodyBytes ||
                (response.total && source.expectedSize && response.total != source.expectedSize)) {
                rejection = "size";
                return false;
            }
            meta.total = response.total ? response.total : source.expectedSize;
            std::strcpy(meta.validator, !separate && *response.validator ? response.validator : pinned);
            if (separate) std::strcpy(meta.wireValidator, response.validator);
            return true;
        };
        const ChunkSink sink = [&](const uint8_t* data, size_t count) {
            const bool whole = response.status == 200;
            const uint32_t limit = whole ? (meta.total ? meta.total : kMaxBodyBytes) : kResumeChunk;
            const uint32_t received = whole ? meta.have + buffered : buffered;
            if (count > limit - received) {
                rejection = "size";
                return false;
            }
            if (count && !chunkBuffer) {
                buffer.reset(new (std::nothrow) uint8_t[kResumeChunk]);
                if (!buffer) {
                    allocationFailed = true;
                    return false;
                }
                chunkBuffer = buffer.get();
            }
            while (count) {
                const size_t copied = std::min(count, size_t(kResumeChunk - buffered));
                std::memcpy(chunkBuffer + buffered, data, copied);
                buffered += static_cast<uint32_t>(copied);
                data += copied;
                count -= copied;
                if (whole && buffered == kResumeChunk && !saveChunk()) {
                    rejection = "storage";
                    return false;
                }
            }
            return true;
        };
        const bool complete = transport.get(source, start, end,
                                            source.sendIfRange ? (separate ? meta.wireValidator : meta.validator) : "",
                                            headers, sink);
        if (allocationFailed) {
            if (failureReason) *failureReason = "memory";
            return FetchOutcome::Failed;
        }
        if (rejection) return fail(rejection);
        if (const char* reason = transport.failureReason()) return fail(reason);
        if (!complete) return pause();
        if (response.status == 200) {
            const uint32_t received = meta.have + buffered;
            if (received < meta.total) return pause();
            if (!received) return fail("size");
            meta.total = received;
            return saveChunk() ? finish() : fail("storage");
        }
        if (!terminated(response.validator, sizeof(response.validator))) return fail("parse");

        // Fixed sources can use their manifest/offer identity when the HTTP
        // response itself does not carry one. Public sources learn it on GET.
        const char* validator = *response.validator ? response.validator : pinned;
        const bool validatorChanged = start && (separate
            ? std::strcmp(meta.wireValidator, response.validator)
            : std::strcmp(meta.validator, validator));
        const bool complete416 = response.status == 416 && start &&
            (total ? start == total && (!response.total || response.total == total)
                   : response.total == start) &&
            (!*response.validator || !validatorChanged);
        if (complete416) {
            meta.total = start;
            return finish();
        }

        const bool shifted = response.status == 206 && response.start != start;
        const bool changed206 = response.status == 206 && start &&
            (validatorChanged || (meta.total && response.total && response.total != meta.total));
        if (shifted || changed206 || (response.status == 416 && start)) {
            if (restarted) return fail(response.status == 416 ? "status-416" : "parse");
            reset();
            progressed = false;
            restarted = true;
            continue;
        }

        if (response.status != 206) return FetchOutcome::Failed;
        if (!separate && *pinned && std::strcmp(pinned, validator)) return fail("validator");

        const uint32_t responseTotal = response.total;
        if (!responseTotal || responseTotal > kMaxBodyBytes ||
            (source.expectedSize && responseTotal != source.expectedSize)) return fail("size");

        if (response.end < start || response.end > end || response.end >= responseTotal) {
            return fail("parse");
        }
        const uint32_t expectedBytes = response.end - start + 1;
        if (buffered < expectedBytes) return pause();
        if (buffered > expectedBytes) return fail("size");
        if (response.end + 1 < responseTotal && !*validator && !*digest) return fail("validator");

        meta.total = responseTotal;
        std::strcpy(meta.validator, separate ? pinned : validator);
        if (separate) std::strcpy(meta.wireValidator, response.validator);
        if (!saveChunk()) return fail("storage");
        if (meta.have == meta.total) return finish();
    }
    return pause();
}
