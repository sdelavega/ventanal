// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
//
// A bounded HTTPS GET for public APIs: certificate bundle, the product
// User-Agent and no credentials, no redirects, a size cap and an overall
// deadline, feeding the caller's watchdog between reads.
#pragma once

#include <cstddef>
#include <cstdint>

#ifndef VENTANAL_FIRMWARE_VERSION
#error "VENTANAL_FIRMWARE_VERSION must be defined by the build"
#endif

inline constexpr char publicUserAgent[] = "Ventanal/" VENTANAL_FIRMWARE_VERSION;

struct HttpsResult {
    int status = -1;        // HTTP status, or -1 when no response arrived
    size_t length = 0;      // bytes stored in the buffer (NUL-terminated after them)
    bool truncated = false; // the body did not fit in the buffer: treat as failure
};

// buffer receives the body plus a terminating NUL, so cap must be > 0.
HttpsResult httpsGet(const char* url, char* buffer, size_t cap, uint32_t deadlineMs, void (*feed)());
