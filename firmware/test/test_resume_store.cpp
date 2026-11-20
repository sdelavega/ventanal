// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
#include "../common/resume/fs_store.h"
#include "../common/resume/resume.h"
#include "../common/resume/storage.h"
#include <LittleFS.h>
#include <cassert>
#include <cstdio>

namespace {
constexpr uint32_t kNow = 1704067200 + 10 * 86400;
constexpr uint8_t kBody[] = {'a', 'b', 'c'};
constexpr char kUrl[] = "https://example.test/frame";

PartialMeta metaAt(uint32_t epoch, const char* validator = "live") {
    PartialMeta meta{};
    std::strcpy(meta.url, kUrl);
    std::strcpy(meta.validator, validator);
    meta.have = sizeof(kBody);
    meta.total = sizeof(kBody);
    meta.touchedAt = epoch;
    return meta;
}

void seed(FsStore& store, const char* target, uint32_t epoch, const char* validator = "live") {
    assert(store.append(target, kBody, sizeof(kBody)));
    assert(store.writeMeta(target, metaAt(epoch, validator)));
}

void fs_store_roundtrip() {
    FsStore store(LittleFS, "/roundtrip");
    seed(store, "/frame", kNow);
    assert(store.append("/frame", kBody, sizeof(kBody)));
    assert(store.size("/frame") == 6);
    assert(store.truncate("/frame", 3));
    PartialMeta meta{};
    assert(store.readMeta("/frame", meta));
    assert(encodePartialMeta(meta) == encodePartialMeta(metaAt(kNow)));
    uint8_t digest[32]{};
    assert(store.sha256("/frame", digest));
    constexpr uint8_t expected[] = {
        0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea, 0x41, 0x41, 0x40, 0xde, 0x5d, 0xae, 0x22, 0x23,
        0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17, 0x7a, 0x9c, 0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad
    };
    assert(!std::memcmp(digest, expected, sizeof(expected)));
    files["/roundtrip/frame"] = {0};
    assert(store.commit("/frame"));
    assert(files.at("/roundtrip/frame") == std::vector<uint8_t>({'a', 'b', 'c'}));
    assert(store.size("/frame") == -1);
    assert(!store.readMeta("/frame", meta));
}

class WholeResponse : public RangeTransport {
    bool get(const ResumeSource&, uint32_t, uint32_t, const char*,
             const ResponseHeaders& headers, const ChunkSink& sink) override {
        return headers({200, 0, 0, sizeof(kBody), "live"}) && sink(kBody, sizeof(kBody));
    }
};

void write_failure_abandons_partial() {
    FsStore store(LittleFS, "/failure");
    seed(store, "frame", kNow);
    files["/failure/frame"] = {42};
    writeLimit = 1;
    assert(!store.append("frame", kBody, sizeof(kBody)));
    WholeResponse transport;
    ResumeSource source{kUrl, "live", nullptr, sizeof(kBody), false};
    const auto now = []() -> uint32_t { return 0; };
    assert(resumeFetch(transport, store, "frame", source, 1, now, kNow, nullptr) == FetchOutcome::Failed);
    writeLimit = SIZE_MAX;
    assert(!LittleFS.exists("/failure/frame.part"));
    assert(!LittleFS.exists("/failure/frame.meta"));
    assert(files.at("/failure/frame") == std::vector<uint8_t>({42}));
}

void sd_gone_falls_back_to_flash() {
    static fs::FS sd;
    writeLimit = 511;
    assert(!probePartialStorage(sd));
    assert(sd.files.empty());
    writeLimit = SIZE_MAX;
    assert(probePartialStorage(sd));
    assert(sd.files.empty());
    setPartialSdFilesystem(sd);
    selectPartialStore(true);
    assert(Serial.output == "storage partials=sd\n");
    auto& failedStore = partialStore();
    assert(failedStore.append("sd-frame", kBody, sizeof(kBody)));
    assert(sd.exists("/partials/sd-frame.part"));
    assert(!LittleFS.exists("/partials/sd-frame.part"));
    sd.replaceOnRename = false;
    sd.files["/partials/sd-frame"] = {42};
    assert(failedStore.commit("sd-frame"));
    assert(sd.files.at("/partials/sd-frame") == std::vector<uint8_t>({'a', 'b', 'c'}));
    assert(!sd.exists("/partials/sd-frame.part"));
    assert(&partialStore() == &failedStore);
    sd.offline = true;
    assert(failedStore.size("removed-card") == -1);
    assert(!failedStore.append("removed-card", kBody, sizeof(kBody)));
    assert(Serial.output == "storage partials=sd\nstorage partials=flash\n");
    auto& flash = partialStore();
    assert(&flash != &failedStore);
    assert(flash.append("flash-frame", kBody, sizeof(kBody)));
    assert(LittleFS.exists("/partials/flash-frame.part"));

    sd.offline = false;
    setPartialSdFilesystem(sd);
    selectPartialStore(true);
    writeLimit = 1;
    assert(!partialStore().append("full-card", kBody, sizeof(kBody)));
    writeLimit = SIZE_MAX;
    assert(&partialStore() == &flash);
}

bool live(const PartialMeta& meta) { return std::strcmp(meta.validator, "live") == 0; }

void sweep_removes_old_and_orphaned() {
    FsStore store(LittleFS, "/partials");
    selectPartialStore(false);
    seed(store, "old", kNow - 8 * 86400);
    seed(store, "deadline", kNow - 7 * 86400);
    seed(store, "fresh", kNow - 7 * 86400 + 1);
    seed(store, "future", kNow + 1);
    seed(store, "dead", kNow, "dead");
    assert(store.append("orphan", kBody, sizeof(kBody)));
    assert(store.writeMeta("sidecar", metaAt(kNow)));
    assert(store.append("corrupt", kBody, sizeof(kBody)));
    files["/partials/corrupt.meta"] = {0};
    files["/partials/committed"] = {42};
    sweepPartials(kNow, live);
    for (const char* target : {"old", "deadline", "dead", "orphan", "sidecar", "corrupt"}) {
        assert(store.size(target) == -1);
        PartialMeta meta{};
        assert(!store.readMeta(target, meta));
    }
    assert(store.size("fresh") == 3);
    assert(store.size("future") == 3);
    assert(files.at("/partials/committed") == std::vector<uint8_t>({42}));
}

void sweep_needs_valid_time() {
    FsStore store(LittleFS, "/partials");
    seed(store, "invalid-clock", 0, "dead");
    assert(store.append("orphan", kBody, sizeof(kBody)));
    sweepPartials(1000, live);
    assert(store.size("invalid-clock") == 3);
    assert(store.size("orphan") == 3);
}
}

int main() {
    fs_store_roundtrip();
    write_failure_abandons_partial();
    sd_gone_falls_back_to_flash();
    sweep_removes_old_and_orphaned();
    sweep_needs_valid_time();
    std::puts("resume store: 5 cases passed");
}
