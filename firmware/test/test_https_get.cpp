// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
#include "../common/https_get.h"
#include "fakes/esp_http_client.h"
#include <cassert>
#include <cstdio>
#include <initializer_list>

int main() {
    char body[8193];
    for (const int stalled : {0, -1}) {
        fakeHttps = {100, 200, 50, 0, stalled};
        const auto partial = httpsGet("https://fixture.test", body, sizeof body, 1000, nullptr);
        assert(partial.status == -1 && !partial.truncated && partial.length == 50 && body[50] == '\0');
    }

    fakeHttps = {8192, 206, 8192};
    const auto tail = httpsGet("https://fixture.test", body, sizeof body, 1000, nullptr, "bytes=-8192");
    assert(!strcmp(fakeHttps.range, "bytes=-8192"));
    assert(tail.status == 206 && !tail.truncated && tail.length == 8192 && body[8192] == '\0');

    fakeHttps = {0, 200, 25};
    const auto incompleteChunked = httpsGet("https://fixture.test", body, sizeof body, 1000, nullptr);
    assert(incompleteChunked.status == -1);
    fakeHttps = {0, 200, 25};
    fakeHttps.complete = true;
    const auto chunked = httpsGet("https://fixture.test", body, 26, 1000, nullptr);
    assert(chunked.status == 200 && !chunked.truncated && chunked.length == 25);

    fakeHttps = {100, 200, 100};
    const auto oversized = httpsGet("https://fixture.test", body, 100, 1000, nullptr);
    assert(oversized.status == 200 && oversized.truncated && oversized.length == 0);
    puts("HTTPS GET: stalled bodies, exact ranged tails, chunked completion and size cap passed");
}
