// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
#include "../common/resume/resume.h"
#include <algorithm>
#include <cassert>
#include <cstdarg>
#include <cstdio>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace {

constexpr char kTarget[] = "/frame";
constexpr char kUrl[] = "https://example.test/frame";
constexpr uint32_t kEpoch = 1704067200;
constexpr uint32_t kBodySize = 100 * 1024;
constexpr char kDigest[] = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
uint32_t clockMs;
std::vector<std::string> logs;

uint32_t nowMs() { return clockMs; }

void logLine(const char* format, ...) {
    char line[160];
    va_list args;
    va_start(args, format);
    std::vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    logs.emplace_back(line);
}

struct Exchange {
    uint32_t start;
    uint32_t end;
    const char* ifRange;
    int status = 206;
    const char* validator = "one";
    uint32_t responseStart = UINT32_MAX;
    uint32_t cutAfter = UINT32_MAX;
    bool unknownTotal = false;
};

class ScriptedTransport : public RangeTransport {
public:
    std::vector<uint8_t> body;
    std::vector<Exchange> script;
    size_t requests = 0;
    std::function<void(uint32_t)> afterPacket;

    explicit ScriptedTransport(std::vector<Exchange> steps, uint32_t bytes = kBodySize)
        : body(bytes), script(std::move(steps)) {
        for (size_t i = 0; i < body.size(); ++i) {
            body[i] = static_cast<uint8_t>(i * 37 + i / 257);
        }
    }

    bool get(const ResumeSource& source, uint32_t start, uint32_t end, const char* ifRange,
             const ResponseHeaders& headers, const ChunkSink& sink) override {
        assert(std::string(source.url) == kUrl);
        assert(requests < script.size());
        const Exchange& step = script[requests++];
        assert(start == step.start && end == step.end);
        assert(std::string(ifRange ? ifRange : "") == step.ifRange);
        clockMs += 10;

        RangeResponse response{};
        response.status = step.status;
        if (step.status == -1) return false;
        const uint32_t bodySize = static_cast<uint32_t>(body.size());
        response.total = step.unknownTotal ? 0 : bodySize;
        std::snprintf(response.validator, sizeof(response.validator), "%s", step.validator);
        if (step.status == 416) return headers(response);
        response.start = step.status == 200 ? 0 : start;
        if (step.responseStart != UINT32_MAX) response.start = step.responseStart;
        response.end = step.status == 200 ? bodySize - 1 : std::min(end, bodySize - 1);
        if (!headers(response)) return false;
        const uint32_t length = response.end - response.start + 1;
        const uint32_t stop = response.start + std::min(step.cutAfter, length);
        for (uint32_t at = response.start; at < stop;) {
            const uint32_t count = std::min(997u, stop - at);
            if (!sink(body.data() + at, count)) return false;
            at += count;
            if (afterPacket) afterPacket(at - response.start);
        }
        return step.cutAfter >= length;
    }
};

class RecordingStore : public MemoryStore {
public:
    std::vector<size_t> writes;

    bool append(const char* target, const uint8_t* data, size_t size) override {
        writes.push_back(size);
        return MemoryStore::append(target, data, size);
    }
};

class Fixture {
public:
    RecordingStore store;
    ResumeSource source{kUrl, "one", "", kBodySize, false};

    Fixture() {
        clockMs = 0;
        logs.clear();
    }

    void seed(uint32_t fileSize, uint32_t have, const std::vector<uint8_t>& body) {
        PartialMeta meta{};
        std::snprintf(meta.url, sizeof(meta.url), "%s", kUrl);
        std::snprintf(meta.validator, sizeof(meta.validator), "%s", "one");
        meta.total = static_cast<uint32_t>(body.size());
        meta.have = have;
        meta.touchedAt = kEpoch;
        assert(store.append(kTarget, body.data(), fileSize));
        assert(store.writeMeta(kTarget, meta));
        store.writes.clear();
    }

    FetchOutcome fetch(ScriptedTransport& transport, uint32_t budget = 100) {
        const FetchOutcome result = resumeFetch(transport, store, kTarget, source, budget,
                                                nowMs, kEpoch, logLine);
        assert(transport.requests == transport.script.size());
        return result;
    }

    void checkCommit(const ScriptedTransport& transport) {
        const auto* committed = store.committed(kTarget);
        assert(committed && *committed == transport.body);
        PartialMeta meta{};
        assert(!store.readMeta(kTarget, meta) && store.size(kTarget) == -1);
        assert(logs.back().find("resume done target=/frame bytes=") == 0);
    }
};

void fresh_download_completes_in_chunks() {
    Fixture f;
    f.source.sha256Hex = "8895c8cbbf1367827685af670f18d16c49802d3470156f95f378a8e8834a5d79";
    ScriptedTransport transport({{0, 32767, ""}, {32768, 65535, ""},
                                 {65536, 98303, ""}, {98304, 102399, ""}});
    assert(f.fetch(transport) == FetchOutcome::Complete);
    f.checkCommit(transport);
    assert((f.store.writes == std::vector<size_t>{32768, 32768, 32768, 4096}));
    assert((logs == std::vector<std::string>{"resume done target=/frame bytes=102400 ms=40"}));
}

void budget_stops_with_progress_and_resumes() {
    Fixture f;
    ScriptedTransport first({{0, 32767, ""}, {32768, 65535, ""}});
    assert(f.fetch(first, 20) == FetchOutcome::Progress);
    PartialMeta meta{};
    assert(f.store.readMeta(kTarget, meta) && meta.have == 65536);
    assert(meta.touchedAt == kEpoch);
    ScriptedTransport second({{65536, 98303, ""}, {98304, 102399, ""}});
    assert(f.fetch(second) == FetchOutcome::Complete);
    f.checkCommit(second);
    assert((logs == std::vector<std::string>{"resume target=/frame from=65536 total=102400",
                                             "resume done target=/frame bytes=102400 ms=20"}));
}

void validator_change_restarts() {
    Fixture f;
    f.source.validator = "";
    f.source.sendIfRange = true;
    ScriptedTransport first({{0, 32767, ""}});
    assert(f.fetch(first, 10) == FetchOutcome::Progress);
    ScriptedTransport second({{32768, 65535, "one", 206, "two"},
                              {0, 32767, "", 206, "two"}, {32768, 65535, "two", 206, "two"},
                              {65536, 98303, "two", 206, "two"}, {98304, 102399, "two", 206, "two"}});
    second.body.front() ^= 0xff;
    assert(f.fetch(second) == FetchOutcome::Complete);
    f.checkCommit(second);

    // A different Content-Range start invalidates a partial even with the same validator.
    Fixture wrongStart;
    ScriptedTransport shifted({{32768, 65535, "", 206, "one", 32769},
                               {0, 32767, ""}, {32768, 65535, ""},
                               {65536, 98303, ""}, {98304, 102399, ""}});
    wrongStart.seed(32768, 32768, shifted.body);
    assert(wrongStart.fetch(shifted) == FetchOutcome::Complete);
    wrongStart.checkCommit(shifted);
}

void if_range_full_body_restarts_from_zero() {
    Fixture f;
    f.source = {kUrl, "", "", 0, true};
    ScriptedTransport old({});
    f.seed(32768, 32768, old.body);
    ScriptedTransport transport({{32768, 65535, "one", 200, "two"}}, kBodySize - 11);
    transport.body.front() ^= 0xff;
    assert(f.fetch(transport) == FetchOutcome::Complete);
    f.checkCommit(transport);
    assert((f.store.writes == std::vector<size_t>{32768, 32768, 32768, 4085}));
}

void ignored_range_is_whole_download() {
    Fixture f;
    ScriptedTransport transport({{65536, 98303, "", 200}});
    f.seed(65536, 65536, transport.body);
    transport.body.front() ^= 0xff;
    assert(f.fetch(transport) == FetchOutcome::Complete);
    f.checkCommit(transport);
}

void whole_body_streams_in_chunks() {
    constexpr uint32_t total = 5 * kResumeChunk + 100;
    Fixture f;
    f.source = {kUrl, "", "", 0, true};
    ScriptedTransport transport({{kResumeChunk, 2 * kResumeChunk - 1, "one", 200, "two"}}, total);
    f.seed(kResumeChunk, kResumeChunk, transport.body);
    transport.body.front() ^= 0xff;
    uint32_t peakPending = 0;
    transport.afterPacket = [&](uint32_t delivered) {
        const int64_t size = f.store.size(kTarget);
        const uint32_t persisted = size < 0 ? 0 : static_cast<uint32_t>(size);
        assert(persisted == delivered / kResumeChunk * kResumeChunk);
        peakPending = std::max(peakPending, delivered - persisted);
        PartialMeta meta{};
        assert(f.store.readMeta(kTarget, meta) == (persisted != 0));
        if (persisted) {
            assert(meta.have == persisted && meta.total == total);
            assert(std::string(meta.validator) == "two");
        }
    };
    assert(f.fetch(transport) == FetchOutcome::Complete);
    f.checkCommit(transport);
    assert(peakPending <= kResumeChunk);
    assert((f.store.writes == std::vector<size_t>{kResumeChunk, kResumeChunk, kResumeChunk,
                                                 kResumeChunk, kResumeChunk, 100}));
}

void interrupted_whole_body_resumes() {
    constexpr uint32_t cut = 2 * kResumeChunk;
    for (bool unknownTotal : {false, true}) {
        Fixture f;
        f.source = {kUrl, "", "", 0, true};
        ScriptedTransport first({{0, 32767, "", 200, "learned", UINT32_MAX, cut, unknownTotal}});
        assert(f.fetch(first) == FetchOutcome::Progress);
        PartialMeta meta{};
        assert(f.store.readMeta(kTarget, meta) && meta.have == cut);
        assert(meta.total == (unknownTotal ? 0 : kBodySize));
        assert(std::string(meta.validator) == "learned");
        assert(f.store.size(kTarget) == cut);
        ScriptedTransport second({{cut, 98303, "learned", 206, "learned"},
                                  {98304, 102399, "learned", 206, "learned"}});
        assert(f.fetch(second) == FetchOutcome::Complete);
        f.checkCommit(second);
    }
}

void interrupted_whole_body_without_identity_restarts() {
    Fixture f;
    f.source = {kUrl, "", "", 0, true};
    ScriptedTransport first({{0, 32767, "", 200, "", UINT32_MAX, 2 * kResumeChunk + 100}});
    assert(f.fetch(first) == FetchOutcome::Progress);
    PartialMeta meta{};
    assert(f.store.readMeta(kTarget, meta) && meta.have == 2 * kResumeChunk && !*meta.validator);
    assert(f.store.size(kTarget) == meta.have);

    ScriptedTransport second({{0, 32767, "", 200, "", UINT32_MAX, UINT32_MAX, true}});
    second.body.front() ^= 0xff;
    assert(f.fetch(second) == FetchOutcome::Complete);
    f.checkCommit(second);
}

void torn_partial_resumes_at_sidecar() {
    Fixture f;
    ScriptedTransport transport({{65536, 98303, ""}, {98304, 102399, ""}});
    f.seed(70000, 65536, transport.body);
    assert(f.fetch(transport) == FetchOutcome::Complete);
    f.checkCommit(transport);

    Fixture shortFile;
    ScriptedTransport shortTransport({{60000, 92767, ""}, {92768, 102399, ""}});
    shortFile.seed(60000, 65536, shortTransport.body);
    PartialMeta meta{};
    assert(shortFile.store.readMeta(kTarget, meta));
    auto sidecar = encodePartialMeta(meta);
    assert(std::string(sidecar.begin(), sidecar.begin() + 4) == "VRP1");
    assert(sidecar.size() == 348 && sidecar[332] == 0x00 && sidecar[333] == 0x90 &&
           sidecar[334] == 0x01);
    sidecar[8] ^= 1;
    assert(!decodePartialMeta(sidecar.data(), sidecar.size(), meta));
    assert(shortFile.fetch(shortTransport) == FetchOutcome::Complete);
    shortFile.checkCommit(shortTransport);
}

void sha_mismatch_discards() {
    Fixture f;
    f.source.expectedSize = 3;
    f.source.sha256Hex = "aa7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
    ScriptedTransport transport({{0, 2, ""}}, 3);
    transport.body = {'a', 'b', 'c'};
    assert(f.fetch(transport) == FetchOutcome::Failed);
    assert(f.store.committed(kTarget) == nullptr && f.store.size(kTarget) == -1);
    PartialMeta meta{};
    assert(!f.store.readMeta(kTarget, meta));
}

void no_response_is_no_progress() {
    Fixture f;
    ScriptedTransport transport({{32768, 65535, "", -1}});
    f.seed(32768, 32768, transport.body);
    assert(f.fetch(transport) == FetchOutcome::NoProgress);
    PartialMeta meta{};
    assert(f.store.readMeta(kTarget, meta) && meta.have == 32768);
    assert(f.store.size(kTarget) == 32768 && f.store.writes.empty());

    ScriptedTransport interrupted({{32768, 65535, "", 206, "one", UINT32_MAX, 997}});
    assert(f.fetch(interrupted) == FetchOutcome::NoProgress);
    assert(f.store.readMeta(kTarget, meta) && meta.have == 32768);
    assert(f.store.size(kTarget) == 32768 && f.store.writes.empty());
}

void complete_partial_416_verifies() {
    Fixture f;
    f.source.expectedSize = 3;
    f.source.sha256Hex = kDigest;
    ScriptedTransport transport({{3, 3, "", 416}}, 3);
    transport.body = {'a', 'b', 'c'};
    f.seed(3, 3, transport.body);
    assert(f.fetch(transport) == FetchOutcome::Complete);
    assert(f.store.writes.empty());
    f.checkCommit(transport);
}

}  // namespace

int main() {
    fresh_download_completes_in_chunks();
    budget_stops_with_progress_and_resumes();
    validator_change_restarts();
    if_range_full_body_restarts_from_zero();
    ignored_range_is_whole_download();
    whole_body_streams_in_chunks();
    interrupted_whole_body_resumes();
    interrupted_whole_body_without_identity_restarts();
    torn_partial_resumes_at_sidecar();
    sha_mismatch_discards();
    no_response_is_no_progress();
    complete_partial_416_verifies();
    std::puts("test_resume: 12 tests passed");
}
