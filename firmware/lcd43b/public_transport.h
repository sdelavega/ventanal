// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
#pragma once

#include "../common/https_get.h"

namespace lcd {

class PublicRangeTransport : public RangeTransport {
public:
    PublicRangeTransport(const char* url, size_t cap, uint32_t budgetMs, uint32_t (*nowMs)(), void (*feed)())
        : url_(url), cap_(cap), budgetMs_(budgetMs), nowMs_(nowMs), feed_(feed), started_(nowMs()) {}

    bool get(const ResumeSource&, uint32_t start, uint32_t end, const char* ifRange,
             const ResponseHeaders& headers, const ChunkSink& sink) override {
        reason = "budget";
        const uint32_t elapsed = nowMs_() - started_;
        if (elapsed >= budgetMs_) return false;
        reason = nullptr;
        RangeResponse response{};
        const auto acceptHeaders = [&](const RangeResponse& incoming) {
            total = incoming.total;
            if (total > cap_) return rejectSize();
            // Keep transient server responses in the same category as a timeout.
            if (incoming.status == 408 || incoming.status == 429 || incoming.status >= 500) return false;
            return headers(incoming);
        };
        size_t bodyBytes = 0;
        const auto acceptBytes = [&](const uint8_t* bytes, size_t count) {
            if (count > cap_ - bodyBytes) return rejectSize();
            bodyBytes += count;
            return sink(bytes, count);
        };
        const auto got = httpsGetRange(url_, start, end, ifRange, acceptBytes, budgetMs_ - elapsed,
                                       feed_, response, acceptHeaders);
        status = response.status;
        bytesRead += got.length;
        if (got.status == 200 && !total) total = got.length;
        if (got.truncated) return rejectSize();
        if (got.status < 0 && !reason && (status <= 0 || status == 200 || status == 206))
            reason = status <= 0 ? "transport" : "interrupted";
        return got.status > 0;
    }

    size_t bytesRead = 0;
    uint32_t total = 0;
    int status = 0;
    const char* reason = nullptr;
    bool malformed = false;

private:
    bool rejectSize() {
        reason = "size";
        malformed = true;
        return false;
    }

    const char* url_;
    size_t cap_;
    uint32_t budgetMs_;
    uint32_t (*nowMs_)();
    void (*feed_)();
    uint32_t started_;
};

}  // namespace lcd
