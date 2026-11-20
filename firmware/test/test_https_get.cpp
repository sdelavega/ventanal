// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
#include "../common/https_get.h"
#include "../lcd43b/public_transport.h"
#include "fakes/esp_http_client.h"
#include <cassert>
#include <cstdio>
#include <initializer_list>

static void streaming_ranges() {
    constexpr char url[] = "https://fixture.test/piece.jpg";
    constexpr char modified[] = "Thu, 19 Nov 2026 12:00:00 GMT";
    const ResumeSource source{url, nullptr, nullptr, 0, true};
    const auto clock = []() -> uint32_t { return 0; };
    lcd::PublicRangeTransport transport(url, 500 * 1024, 1000, clock, nullptr);
    RangeResponse received{};
    size_t copied = 0;
    const auto headers = [&](const RangeResponse& response) {
        assert(fakeHttps.read == 0 && copied == 0);
        received = response;
        return true;
    };
    const auto sink = [&](const uint8_t*, size_t count) {
        assert(received.status);
        copied += count;
        return true;
    };

    fakeHttps = {8, 206, 8};
    fakeHttps.contentRange = "bytes 32768-32775/32776";
    fakeHttps.etag = "\"strong\"";
    fakeHttps.modified = modified;
    assert(transport.get(source, 32768, 32775, "\"strong\"", headers, sink));
    assert(fakeRange == "bytes=32768-32775" && fakeIfRange == "\"strong\"");
    assert(received.total == 32776 && received.start == 32768 && received.end == 32775 && copied == 8);
    assert(!strcmp(received.validator, "\"strong\""));

    copied = 0;
    fakeHttps.read = 0;
    fakeHttps.etag = "W/\"weak\"";
    assert(transport.get(source, 32768, 32775, modified, headers, sink));
    assert(fakeIfRange == modified && !strcmp(received.validator, modified));

    copied = 0;
    fakeHttps = {32, 200, 32};
    assert(transport.get(source, 32768, 32775, "\"old\"", headers, sink));
    assert(received.status == 200 && received.total == 32 && copied == 32);

    copied = 0;
    fakeHttps = {0, 416, 0};
    fakeHttps.contentRange = "bytes */32776";
    assert(transport.get(source, 32776, 32776, modified, headers, sink));
    assert(received.status == 416 && received.total == 32776 && copied == 0);

    fakeHttps = {8, 206, 8};
    fakeHttps.contentRange = "bytes 32768-32775/32775";
    assert(!transport.get(source, 32768, 32775, modified, headers, sink));
    assert(transport.malformed && fakeHttps.read == 0);

    fakeHttps = {8, 206, 3};
    fakeHttps.contentRange = "bytes 32768-32775/32776";
    assert(!transport.get(source, 32768, 32775, modified, headers, sink));
    assert(copied == 3);

    fakeHttps = {0, 200, 13};
    fakeHttps.complete = true;
    RangeResponse whole{};
    const auto getWhole = httpsGetRange(url, 0, 32767, nullptr,
        [](const uint8_t*, size_t) { return true; }, 1000, nullptr, whole);
    assert(getWhole.status == 200 && getWhole.length == 13 && whole.total == 0);
    puts("HTTPS streaming ranges: headers before body, validators, whole response and interruptions passed");
}

static void github_asset_redirects_keep_range_and_tls() {
    constexpr char url[] = "https://github.com/release.bin";
    constexpr char asset[] = "https://release-assets.githubusercontent.com/file?token=fixture";
    FakeHttpsResponse redirected{3, 206, 3};
    redirected.contentRange = "bytes 32768-32770/32771";
    fakeHttps = {0, 302};
    fakeHttps.location = asset;
    fakeRedirectResponse = &redirected;
    RangeResponse response{};
    size_t bytes = 0;
    const auto sink = [&](const uint8_t*, size_t count) { bytes += count; return true; };
    const auto headers = [](const RangeResponse& head) { return head.status == 206; };
    const auto result = githubGetRange(url, 32768, 32770, "\"tag\"", sink, 1000, nullptr,
                                       response, headers, false);
    assert(result.status == 206 && bytes == 3 && fakeLastUrl == asset);
    assert(fakeRange == "bytes=32768-32770" && fakeIfRange == "\"tag\"");
    assert(fakeHttpsConfig.crt_bundle_attach && fakeHttpsConfig.disable_auto_redirect);

    fakeHttps.location = "https://release-assets.githubusercontent.com.evil.test/file";
    bytes = 0;
    const auto rejected = githubGetRange(url, 0, 2, nullptr, sink, 1000, nullptr,
                                         response, headers, false);
    assert(rejected.status == 302 && bytes == 0 && fakeLastUrl == url);
    fakeRedirectResponse = nullptr;
}

int main() {
    streaming_ranges();
    github_asset_redirects_keep_range_and_tls();
    char body[8193];
    fakeHttps = {};
    fakeHttps.openResult = -1;
    const auto connectFailed = httpsGet("https://fixture.test", body, sizeof body, 1000, nullptr);
    assert(connectFailed.status == -1 && connectFailed.length == 0 && body[0] == '\0');

    fakeHttps = {-1, 0};
    const auto headerFailed = httpsGet("https://fixture.test", body, sizeof body, 1000, nullptr);
    assert(headerFailed.status == -1 && headerFailed.length == 0 && body[0] == '\0');

    fakeHttps = {0, 0};
    const auto missingStatus = httpsGet("https://fixture.test", body, sizeof body, 1000, nullptr);
    assert(missingStatus.status == -1 && missingStatus.length == 0 && body[0] == '\0');

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
    puts("HTTPS GET: missing responses, stalled bodies, exact ranged tails, chunked completion and size cap passed");
}
