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
    uint8_t saved[sizeof(kBody)]{};
    assert(store.readCommitted("frame", saved, sizeof(saved)));
    assert(!std::memcmp(saved, kBody, sizeof(saved)));
    assert(store.openCommitted("frame"));  // a compose or publish attempt can be retried
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

void prune_old_committed_sun_files() {
    FsStore store(LittleFS, "/sun");
    for (const char* target : {"sdo-9999", "sdo-20775", "sdo-20776", "art-met-1"}) {
        seed(store, target, kNow);
        assert(store.commit(target));
    }
    seed(store, "sdo-20777", kNow);
    assert(store.retainPreviousCommitted("sdo-", 20777) == "sdo-20776");
    assert(!store.openCommitted("sdo-9999"));
    assert(!store.openCommitted("sdo-20775"));
    auto sun = store.openCommitted("sdo-20776");
    assert(sun && sun.size() == sizeof(kBody));
    assert(store.openCommitted("art-met-1"));
    PartialMeta meta{};
    assert(store.size("sdo-20777") == sizeof(kBody) && store.readMeta("sdo-20777", meta));
    assert(store.commit("sdo-20777"));
    assert(store.retainPreviousCommitted("sdo-", 20777) == "sdo-20776");
    assert(store.openCommitted("sdo-20777") && store.openCommitted("sdo-20776"));
    assert(store.retainPreviousCommitted("sdo-", 20778) == "sdo-20777");
    assert(!store.openCommitted("sdo-20776"));
}
}

int main() {
    fs_store_roundtrip();
    write_failure_abandons_partial();
    sd_gone_falls_back_to_flash();
    sweep_removes_old_and_orphaned();
    sweep_needs_valid_time();
    prune_old_committed_sun_files();
    std::puts("resume store: 6 cases passed");
}
