// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
#pragma once

#include "https_get.h"

class OtaGithubTransport : public RangeTransport {
public:
    OtaGithubTransport(const char* url, uint32_t budget, uint32_t (*now)(), void (*feed)(),
                       bool benchHttp = false)
        : url_(url), budget_(budget), now_(now), feed_(feed), started_(now()), benchHttp_(benchHttp) {}

    bool get(const ResumeSource&, uint32_t start, uint32_t end, const char* ifRange,
             const ResponseHeaders& headers, const ChunkSink& sink) override {
        const uint32_t elapsed = now_() - started_;
        if (elapsed >= budget_) return false;
        RangeResponse response{};
        const auto result = githubGetRange(url_, start, end, ifRange, sink, budget_ - elapsed, feed_,
                                           response, headers, benchHttp_);
        if (result.truncated) failure_ = "size-or-range";
        return result.status > 0 && result.status != 408 && result.status != 429 && result.status < 500;
    }

    const char* failureReason() const override { return failure_; }

private:
    const char* url_;
    uint32_t budget_;
    uint32_t (*now_)();
    void (*feed_)();
    uint32_t started_;
    bool benchHttp_;
    const char* failure_ = nullptr;
};
