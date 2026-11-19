// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
#include "locate.h"

#include "../https_get.h"
#include "portal.h"
#include <memory>

namespace portal {

LocationResult resolveLocation(const char* zip) {
    constexpr size_t cap = 8192;  // both answers are well under 1 KB
    constexpr uint32_t deadlineMs = 10000;
    std::unique_ptr<char[]> body(new (std::nothrow) char[cap]);
    LocationResult r;
    if (!body) return r;  // why = Network: try again
    char url[160];
    zipUrl(zip, url, sizeof url);
    HttpsResult got = httpsGet(url, body.get(), cap, deadlineMs, keepServing);
    r = locationFromZip(got.truncated ? -1 : got.status, body.get());
    if (r.why != LocationResult::Ok) return r;
    timezoneUrl(r.latitude, r.longitude, url, sizeof url);
    got = httpsGet(url, body.get(), cap, deadlineMs, keepServing);
    finishLocation(r, got.truncated ? -1 : got.status, body.get());
    return r;
}

}  // namespace portal
