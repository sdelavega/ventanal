// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
//
// A bounded HTTPS GET for public APIs: certificate bundle, the product
// User-Agent and no credentials, no redirects, a size cap and an overall
// deadline, feeding the caller's watchdog between reads.
#pragma once

#include <cstddef>
#include <cstdint>
#include "resume/resume.h"

#ifndef VENTANAL_FIRMWARE_VERSION
#error "VENTANAL_FIRMWARE_VERSION must be defined by the build"
#endif

inline constexpr char publicUserAgent[] = "Ventanal/" VENTANAL_FIRMWARE_VERSION;

struct HttpsResult {
    int status = -1;        // HTTP status, or -1 on a transport failure
    size_t length = 0;      // bytes stored in the buffer (NUL-terminated after them)
    bool truncated = false; // the body did not fit in the buffer: treat as failure
};

// buffer receives the body plus a terminating NUL, so cap must be > 0.
// A non-null range is sent as the value of the Range request header.
HttpsResult httpsGet(const char* url, char* buffer, size_t cap, uint32_t deadlineMs, void (*feed)(),
                     const char* range = nullptr);

HttpsResult httpsGetRange(const char* url, uint32_t start, uint32_t end, const char* ifRange,
                          const ChunkSink& sink, uint32_t deadlineMs, void (*feed)(), RangeResponse& out);

// The transport applies resume policy to the response before any body is delivered.
HttpsResult httpsGetRange(const char* url, uint32_t start, uint32_t end, const char* ifRange,
                          const ChunkSink& sink, uint32_t deadlineMs, void (*feed)(), RangeResponse& out,
                          const ResponseHeaders& headers);

// Release assets may follow one redirect to GitHub's HTTPS asset hosts.
HttpsResult githubGetRange(const char* url, uint32_t start, uint32_t end, const char* ifRange,
                           const ChunkSink& sink, uint32_t deadlineMs, void (*feed)(), RangeResponse& out,
                           const ResponseHeaders& headers, bool benchHttp);
