// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
#include "../ventanal/ota.h"
#include "../lcd43b/ota.h"
#include "../common/resume/ota_stage.h"
#include <LittleFS.h>
#include <cassert>

static void standalone_checks_github_weekly() {
    constexpr int64_t day = 24 * 60 * 60;
    constexpr int64_t validSince = 1704067200;
    constexpr int64_t last = validSince + day;
    assert(!lcd::githubCheckDue(true, last, last + 6 * day));
    assert(lcd::githubCheckDue(true, last, last + 7 * day));
    assert(!lcd::githubCheckDue(true, 0, validSince - 1));
    assert(lcd::githubCheckDue(true, 0, validSince));
    assert(!lcd::githubCheckDue(false, 0, last + 7 * day));
    assert(!lcd::githubCheckDue(true, last, last - day));
}

namespace {
uint32_t clockMs;
uint32_t stageClock() { return clockMs; }

struct OtaFixture : RangeTransport {
    FakeFS fs;
    FsStore store{fs};
    std::vector<uint8_t> body = std::vector<uint8_t>(2 * kResumeChunk + 1, 42);
    ReleaseInfo release{};
    uint32_t offset = 0;
    unsigned calls = 0;

    OtaFixture() {
        release.major = 12;
        release.minor = 345;
        release.patch = 6789;
        release.size = body.size();
        Sha256 hash;
        hash.update(body.data(), body.size());
        memcpy(release.sha256, hash.digest(), sizeof release.sha256);
    }

    bool get(const ResumeSource&, uint32_t start, uint32_t end, const char* ifRange,
             const ResponseHeaders& headers, const ChunkSink& sink) override {
        assert(!strcmp(ifRange, start ? "\"etag\"" : ""));
        ++calls;
        offset = start;
        ++clockMs;
        return headers({206, start, end, release.size, "\"etag\""}) &&
               sink(body.data() + start, end - start + 1);
    }

    OtaStage stage(const HashFeed& feed = {}) {
        return stageUpdate(*this, release, 1, store, "/firmware.bin", true, stageClock, 1704067200,
                           [&] { return fs.totalBytes() - fs.usedBytes(); }, [] { return false; },
                           nullptr, feed);
    }

    void complete() {
        assert(stage() == OtaStage::Progress);
        assert(stage() == OtaStage::Progress);
        assert(stage() == OtaStage::Complete);
    }
};

void ota_stages_across_passes() {
    OtaFixture f;
    f.complete();
    assert(f.offset == 2 * kResumeChunk);
    assert(f.stage() == OtaStage::Complete && f.calls == 3);
    unsigned installs = 0;
    assert(installStaged(f.store, f.release, {}, [&](fs::File& file) {
        ++installs;
        assert(file.size() == f.release.size);
        return OtaFailure::None;
    }) == OtaFailure::None);
    assert(installs == 1);
}

void ota_new_offer_discards_old_partial() {
    OtaFixture f;
    assert(f.stage() == OtaStage::Progress);
    PartialMeta meta{};
    assert(f.store.readMeta("ota-12.345.6789", meta));
    assert(!strncmp(meta.validator, "12.345.6789:", 12));
    ++f.release.patch;
    assert(f.stage() == OtaStage::Progress && f.offset == 0);
    assert(f.store.size("ota-12.345.6789") < 0);
    f.release.sha256[0] ^= 1;
    assert(f.stage() == OtaStage::Progress && f.offset == 0);
}

void ota_install_requires_verified_partial() {
    OtaFixture f;
    const auto forbidden = [](fs::File&) { assert(false); return OtaFailure::None; };
    assert(installStaged(f.store, f.release, {}, forbidden) == OtaFailure::Digest);
    f.complete();
    f.fs.files.at("/partials/ota-12.345.6789")[0] ^= 1;
    assert(f.stage() == OtaStage::Complete && f.calls == 3);
    assert(installStaged(f.store, f.release, {}, forbidden) == OtaFailure::Digest);
    assert(!f.store.openCommitted("ota-12.345.6789"));
}

void ota_install_failure_keeps_verified_stage() {
    OtaFixture f;
    f.complete();
    const auto target = otaStageTarget(f.release);
    for (const auto result : {OtaFailure::Write, OtaFailure::Download, OtaFailure::None}) {
        assert(f.stage() == OtaStage::Complete && f.calls == 3);
        assert(installStaged(f.store, f.release, {}, [&](fs::File& file) {
            uint8_t byte = 0;
            assert(f.store.readExact(file, &byte, 1));
            assert(byte == f.body.front());
            if (result == OtaFailure::Download) {
                uint8_t tail[2];
                assert(file.seek(file.size() - 1));
                assert(file.read(tail, sizeof tail) < sizeof tail);
            }
            return result;
        }) == result);
        assert(bool(f.store.openCommitted(target.c_str())) == (result != OtaFailure::None));
    }

    f.complete();
    assert(installStaged(f.store, f.release, {}, [](fs::File&) {
        return OtaFailure::Digest;
    }) == OtaFailure::Digest);
    assert(!f.store.openCommitted(target.c_str()));
}

void kept_stage_skips_rehash_before_install() {
    OtaFixture f;
    f.complete();
    const auto reads = readCalls;
    assert(f.stage() == OtaStage::Complete && f.calls == 3);
    assert(readCalls == reads);
    assert(installStaged(f.store, f.release, {}, [&](fs::File&) {
        assert(readCalls - reads == (f.body.size() + kPartialHashBlockBytes - 1) / kPartialHashBlockBytes);
        return OtaFailure::None;
    }) == OtaFailure::None);
}

void repeated_deferral_consumes_attempt() {
    OtaAttemptState state{};
    const char* approval = "0123456789abcdef";
    for (int i = 0; i < 3; ++i) {
        assert(updateOtaAttemptState(state, "2.0.0", approval, true) == OtaFailure::None);
        assert(state.attempts.count == 0);
    }
    for (int i = 1; i <= otaAttemptLimit; ++i) {
        const auto failure = updateOtaAttemptState(state, "2.0.0", approval, true);
        assert(failure == OtaFailure::Deferred && state.attempts.count == i);
        OtaRecord record{};
        setOtaFailure(record, failure, state.attempts.count == otaAttemptLimit);
        assert(validOtaRecord(record));
        char report[otaReportCapacity];
        formatOtaReport(report, record);
        assert(!strcmp(report, i == otaAttemptLimit ? "failed:attempts:deferred" : "failed:deferred"));
    }
    assert(updateOtaAttemptState(state, "2.0.0", approval, true) == OtaFailure::Attempts);
}

void deferral_counter_resets_on_new_offer() {
    OtaAttemptState state{};
    const char* approval = "0123456789abcdef";
    for (int i = 0; i < 3; ++i)
        assert(updateOtaAttemptState(state, "2.0.0", approval, true) == OtaFailure::None);
    assert(updateOtaAttemptState(state, "2.0.0", approval, true) == OtaFailure::Deferred);
    const char* newApproval = "1123456789abcdef";
    assert(updateOtaAttemptState(state, "2.0.0", newApproval, true) == OtaFailure::None);
    assert(state.deferrals == 1 && state.attempts.count == 0);
    assert(updateOtaAttemptState(state, "2.1.0", newApproval, true) == OtaFailure::None);
    assert(state.deferrals == 1 && state.attempts.count == 0);
    assert(updateOtaAttemptState(state, "2.1.0", newApproval, false) == OtaFailure::None);
    assert(state.deferrals == 0 && state.attempts.count == 1);
}

void ota_install_deferred_when_window_short() {
    constexpr uint32_t window = 60000;
    constexpr uint32_t install = 40000;
    assert(otaInstallFitsWindow(1000, 1000, window, install));
    assert(otaInstallFitsWindow(1000, 21000, window, install));
    assert(!otaInstallFitsWindow(1000, 21001, window, install));
    assert(!otaInstallFitsWindow(1000, 61000, window, install));
    assert(!otaInstallFitsWindow(1000, 62000, window, install));
    assert(otaInstallFitsWindow(UINT32_MAX - 999, 19000, window, install));
    assert(!otaInstallFitsWindow(UINT32_MAX - 999, 19001, window, install));
}

void ota_stage_hash_feeds_watchdog() {
    OtaFixture f;
    unsigned feeds = 0;
    const auto feed = [&] { ++feeds; };
    assert(f.stage(feed) == OtaStage::Progress);
    assert(f.stage(feed) == OtaStage::Progress);
    assert(feeds == 0);
    assert(f.stage(feed) == OtaStage::Complete);
    assert(feeds == (f.body.size() + kPartialHashBlockBytes - 1) / kPartialHashBlockBytes);
}
}

int main() {
    // Overnight quiet hours include the start and exclude the end.
    assert(!otaCanInstall(49, 360, 1380, 360));
    assert(!otaCanInstall(100, -1, 1380, 360));
    assert(!otaCanInstall(101, 360, 1380, 360));
    assert(!otaCanInstall(100, 360, -1, 360));
    assert(!otaCanInstall(50, 0, 1380, 360));
    assert(!otaCanInstall(50, 1380, 1380, 360));
    assert(otaCanInstall(50, 360, 1380, 360));
    assert(otaCanInstall(50, 1379, 1380, 360));
    assert(!otaCanInstall(50, 700, 600, 720));
    assert(otaCanInstall(50, 700, 600, 600));

    OtaRecord record{};
    assert(otaBootResult(record, false, OtaSlotState::Other, false) == OtaResult::None);
    record = {OtaResult::Installing, OtaFailure::None, 0x11, "2.0.0"};
    assert(validOtaRecord(record));
    record.version[0] = 'x';
    assert(!validOtaRecord(record));
    record.version[0] = '2';
    // Reset after the journal, before the boot partition was selected.
    assert(otaBootResult(record, false, OtaSlotState::Other, false) == OtaResult::Failed);
    // First boot in the new slot waits for the application check.
    assert(otaBootResult(record, true, OtaSlotState::Pending, true) == OtaResult::Installing);
    assert(otaCanConfirm(true, true, true, true));
    assert(!otaCanConfirm(false, true, true, true));
    assert(!otaCanConfirm(true, false, true, true));
    assert(!otaCanConfirm(true, true, false, true));
    assert(!otaCanConfirm(true, true, true, false));
    // Reset between validating the image and saving the result.
    assert(otaBootResult(record, true, OtaSlotState::Valid, true) == OtaResult::Ok);
    assert(otaBootResult(record, true, OtaSlotState::Valid, false) == OtaResult::Failed);
    assert(otaBootResult(record, false, OtaSlotState::Rejected, false) == OtaResult::RolledBack);

    assert(!otaCanAcknowledge(OtaResult::Installing, false));
    assert(!otaCanAcknowledge(OtaResult::RolledBack, true));
    assert(otaCanAcknowledge(OtaResult::RolledBack, false));
    assert(otaCanAcknowledge(OtaResult::Ok, false));
    assert(otaCanAcknowledge(OtaResult::Failed, true));

    char report[otaReportCapacity];
    formatOtaReport(report, record);
    assert(!report[0]);
    record.result = OtaResult::Ok;
    formatOtaReport(report, record);
    assert(!strcmp(report, "ok:2.0.0"));
    record.result = OtaResult::RolledBack;
    strcpy(record.version, "65535.65535.65535");
    formatOtaReport(report, record);
    assert(!strcmp(report, "rolled_back:65535.65535.65535"));
    record.result = OtaResult::Failed;
    record.failure = OtaFailure::Interrupted;
    formatOtaReport(report, record);
    assert(!strcmp(report, "failed:interrupted"));
    record.failure = OtaFailure::Attempts;
    record.slot = 0;
    formatOtaReport(report, record);
    assert(!strcmp(report, "failed:attempts")); // Legacy record.
    setOtaFailure(record, OtaFailure::Download, true);
    formatOtaReport(report, record);
    assert(!strcmp(report, "failed:attempts:download"));

    OtaAttempts attempts{};
    const char* approval = "0123456789abcdef";
    assert(validOtaApproval(approval) && !validOtaApproval("0123456789abcdeF"));
    for (int i = 0; i < 3; ++i) assert(reserveOtaAttempt(attempts, "2.0.0", approval));
    assert(!reserveOtaAttempt(attempts, "2.0.0", approval));
    assert(reserveOtaAttempt(attempts, "2.0.0", "1123456789abcdef") && attempts.count == 1);
    assert(reserveOtaAttempt(attempts, "2.1.0", "1123456789abcdef") && attempts.count == 2);
    assert(reserveOtaAttempt(attempts, "2.1.0", "") && attempts.count == 1);
    assert(reserveOtaAttempt(attempts, "2.2.0", "") && attempts.count == 1);

    constexpr int64_t now = 4500000000LL;
    constexpr int64_t week = 604800;
    assert(!otaGithubCheckDue(false, now, 0));
    assert(!otaGithubCheckDue(true, 0, 0));
    assert(otaGithubCheckDue(true, now, 0));
    assert(otaGithubCheckDue(true, now, now - week));
    assert(!otaGithubCheckDue(true, now, now - week + 1));
    assert(!otaGithubCheckDue(true, now, now + 1));

    standalone_checks_github_weekly();
    ota_stages_across_passes();
    ota_new_offer_discards_old_partial();
    ota_install_requires_verified_partial();
    ota_install_failure_keeps_verified_stage();
    kept_stage_skips_rehash_before_install();
    repeated_deferral_consumes_attempt();
    deferral_counter_resets_on_new_offer();
    ota_install_deferred_when_window_short();
    ota_stage_hash_feeds_watchdog();
    puts("OTA policy: battery and quiet hours, install recovery, healthy boot, result fields and standalone weekly check passed");
}
