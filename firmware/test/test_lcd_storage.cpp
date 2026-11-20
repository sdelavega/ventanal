// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
#include "../lcd43b/download_sweep.h"
#include "../lcd43b/download_acceptance.h"
#include "../lcd43b/local/art_state.h"
#include <cassert>
#include <cstdio>

using namespace lcd;
namespace {
constexpr uint32_t now = 1704067200 + 10 * 86400;
const uint8_t body[] = {'a', 'b', 'c'};
struct Fixture {
    fs::FS fs;
    FsStore store{fs};
    DownloadSweep sweep;
    void seed(const char* target, bool committed) {
        assert(store.append(target, body, sizeof body));
        PartialMeta meta{};
        std::strcpy(meta.validator, "live");
        meta.have = meta.total = sizeof body;
        meta.touchedAt = now;
        assert(store.writeMeta(target, meta));
        if (committed) assert(store.commit(target));
    }
    bool exists(const char* target) { return fs.exists((std::string("/partials/") + target).c_str()); }
};

void daily_sweep_removes_old_day_sun() {
    Fixture f;
    f.seed("sdo-1", false);
    f.seed("sdo-2", true);
    f.seed("sdo-3", true);
    f.seed("foreign", false);
    f.sweep.run(f.store, now);
    assert(!f.exists("sdo-1.part") && !f.exists("sdo-1.meta") && !f.exists("sdo-2"));
    assert(f.exists("foreign.part") && f.exists("foreign.meta"));
}
void sweep_keeps_newest_sun_and_today() {
    Fixture f;
    const auto today = "sdo-" + std::to_string(now / 86400);
    f.seed("sdo-2", true);
    f.seed(today.c_str(), true);
    f.seed(today.c_str(), false);
    f.sweep.run(f.store, now);
    assert(f.exists("sdo-2") && f.exists(today.c_str()) && f.exists((today + ".part").c_str()));
}
void sweep_removes_unreferenced_art() {
    Fixture f;
    f.seed("art-met-1", true);
    f.seed("art-met-1", false);
    f.seed("art-met-2", false);
    f.seed("art-met-3", true);
    f.sweep.art = {"art-met-2", "art-met-3"};
    f.sweep.run(f.store, now);
    assert(!f.exists("art-met-1") && !f.exists("art-met-1.part"));
    assert(f.exists("art-met-2.part") && f.exists("art-met-3"));
}
void sweep_removes_stale_ota_stage() {
    Fixture f;
    f.seed("ota-1.2.3", true);
    f.seed("ota-2.3.4", true);
    f.fs.modified["/partials/ota-2.3.4"] = now - 7 * 86400;
    f.sweep.otaTarget = "ota-2.3.4";
    f.sweep.otaCommittedMatches = [](FsStore&, const char*) { return true; };
    f.sweep.run(f.store, now);
    assert(!f.exists("ota-1.2.3") && !f.exists("ota-2.3.4"));
}
void sweep_keeps_accepted_ota_stage() {
    Fixture f;
    f.seed("ota-2.3.4", true);
    f.sweep.otaTarget = "ota-2.3.4";
    f.sweep.otaCommittedMatches = [](FsStore&, const char*) { return true; };
    f.sweep.run(f.store, now);
    assert(f.exists("ota-2.3.4"));
}
void sweep_runs_once_per_day() {
    SweepDay day;
    assert(!day.due(42));
    assert(day.due(now));
    assert(!day.due(now + 7199));
    assert(day.due(now + 86400));
    assert(!day.due(now));
}
AcceptedUpdate acceptedOffer() {
    AcceptedUpdate accepted{};
    accepted.source = UpdateSource::Host;
    std::strcpy(accepted.offer.version, "2.3.4");
    accepted.image.size = sizeof body;
    accepted.image.sha256[0] = 42;
    return accepted;
}
void accepted_offer_survives_reboot() {
    Preferences before;
    assert(before.begin("ventanal"));
    assert(saveAcceptedUpdate(before, acceptedOffer()));
    before.end();
    Preferences after;
    assert(after.begin("ventanal"));
    const auto restored = loadAcceptedUpdate(after);
    assert(restored.source == UpdateSource::Host);
    assert(std::strcmp(restored.offer.version, "2.3.4") == 0);
    assert(restored.image.size == sizeof body && restored.image.sha256[0] == 42);
}
void superseded_offer_clears_saved_acceptance() {
    Preferences prefs;
    assert(prefs.begin("ventanal"));
    auto accepted = acceptedOffer();
    assert(saveAcceptedUpdate(prefs, accepted));
    auto current = accepted;
    assert(reconcileAcceptedUpdate(prefs, accepted, current));
    assert(loadAcceptedUpdate(prefs).source == UpdateSource::Host);
    current.image.sha256[0]++;
    assert(reconcileAcceptedUpdate(prefs, accepted, current));
    assert(accepted.source == UpdateSource::None);
    assert(loadAcceptedUpdate(prefs).source == UpdateSource::None);
}
void art_progress_resumes_next_pass() {
    local::ArtState state;
    state.finishPass(20240111, now, false, false);
    assert(state.failedAt == now && state.day == 0);
    state.finishPass(20240111, now + 3600, false, true);
    assert(state.failedAt == 0 && state.day == 0);
    state.finishPass(20240111, now + 4200, true, true);
    assert(state.failedAt == 0 && state.day == 20240111);
}
bool writing = false;
void partial_store_append_enters_scan_scope() {
    Fixture f;
    f.store.setWriteHook([](bool active) { assert(writing != active); writing = active; });
    f.fs.beforeWrite = [] { assert(writing); };
    f.seed("sdo-1", false);
    assert(!writing);
}
}

int main() {
    daily_sweep_removes_old_day_sun();
    sweep_keeps_newest_sun_and_today();
    sweep_removes_unreferenced_art();
    sweep_removes_stale_ota_stage();
    sweep_keeps_accepted_ota_stage();
    sweep_runs_once_per_day();
    accepted_offer_survives_reboot();
    superseded_offer_clears_saved_acceptance();
    art_progress_resumes_next_pass();
    partial_store_append_enters_scan_scope();
    std::puts("LCD downloads: sweep, persisted acceptance, art cadence and write scope passed");
}
