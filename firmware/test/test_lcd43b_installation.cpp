// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
#include "../lcd43b/installation.h"
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>

using namespace lcd;

static void resetStore() {
    settingsPreferences.clear();
    settingsNamespaceExists = false;
    preferences.clear();
    Preferences::failWriteKey.clear();
    Preferences::eraseOnFailedWrite = false;
}

static Installation pairedSeed() {
    Installation seed{};
    strcpy(seed.ssid, "synthetic-network");
    strcpy(seed.password, "synthetic-password");
    strcpy(seed.host, "https://frame.test/");
    strcpy(seed.timezone, "PST8PDT,M3.2.0,M11.1.0");
    strcpy(seed.profile, "desk-display");
    strcpy(seed.pairing, "desk");
    strcpy(seed.certificate, "synthetic first PEM\nsynthetic second PEM");
    memset(seed.deviceKey, 'a', 64);
    return seed;
}

static Installation standaloneRecord() {
    Installation s{};
    strcpy(s.ssid, "synthetic-network");
    strcpy(s.password, "synthetic-password");
    strcpy(s.zip, "12345");
    return s;
}

static bool same(const Installation& a, const Installation& b) {
    return !strcmp(a.ssid, b.ssid) && !strcmp(a.password, b.password) && !strcmp(a.host, b.host)
        && !strcmp(a.timezone, b.timezone) && !strcmp(a.profile, b.profile) && !strcmp(a.pairing, b.pairing)
        && !strcmp(a.certificate, b.certificate) && !strcmp(a.deviceKey, b.deviceKey)
        && !strcmp(a.zip, b.zip) && a.units == b.units && a.clock24h == b.clock24h
        && a.quietStart == b.quietStart && a.quietEnd == b.quietEnd && !strcmp(a.place, b.place)
        && !strcmp(a.callSign, b.callSign) && a.callState == b.callState
        && a.callCheckedAt == b.callCheckedAt && a.forcedRevision == b.forcedRevision
        && (a.latitude == b.latitude || (std::isnan(a.latitude) && std::isnan(b.latitude)));
}

static int slotsStored() {
    return int(settingsPreferences.count(installationSlotA)) + int(settingsPreferences.count(installationSlotB));
}

// The pre-1.1 layout: one 4700-byte schema-1 blob under "installation".
static std::vector<uint8_t> schema1Blob() {
    Installation v1 = pairedSeed();
    v1.schema = 1;
    v1.forcedRevision = 7;
    strcpy(v1.timezone, "CST6CDT,M3.2.0,M11.1.0");
    const auto* bytes = reinterpret_cast<const uint8_t*>(&v1);
    return std::vector<uint8_t>(bytes, bytes + installationSchema1Bytes);
}

static void schema2_layout_pinned() {
    static_assert(installationSchema == 2);
    static_assert(offsetof(Installation, zip) == installationSchema1Bytes);
    assert(sizeof(Installation) == 4792);
}

static void read_only_start_and_seeding() {
    resetStore();
    Installation loaded{};
    assert(!loadInstallation(loaded));
    assert(!settingsNamespaceExists && settingsPreferences.empty());  // no seed: nothing created

    Installation seed{};
    strcpy(seed.ssid, "your-network-name");
    strcpy(seed.host, "http://frame.test");
    assert(!loadInstallation(loaded, &seed, 1));                      // placeholder seed is not written
    assert(!settingsNamespaceExists && settingsPreferences.empty());

    seed = pairedSeed();
    seed.deviceKey[0] = '\0';
    assert(!loadInstallation(loaded, &seed));                         // a partial pairing is unusable
    preferences["lcd-sec-counter"] = {1, 2, 3, 4};
    seed = pairedSeed();
    assert(loadInstallation(loaded, &seed) && same(loaded, seed));
    assert(installationTransportAllowed(loaded, false));
    assert(slotsStored() == 1);
    assert((preferences.at("lcd-sec-counter") == std::vector<uint8_t>{1, 2, 3, 4}));

    Installation plain = loaded;
    strcpy(plain.host, "http://frame.test");
    plain.pairing[0] = plain.certificate[0] = plain.deviceKey[0] = '\0';
    assert(installationTransportAllowed(plain, true) && !installationTransportAllowed(plain, false));
}

static void seed_is_write_once_unless_forced() {
    resetStore();
    Installation seed = pairedSeed(), loaded{};
    assert(loadInstallation(loaded, &seed));
    strcpy(seed.ssid, "replacement-network");
    assert(loadInstallation(loaded, &seed) && !strcmp(loaded.ssid, "synthetic-network"));
    assert(loadInstallation(loaded, nullptr, 99) && !strcmp(loaded.ssid, "synthetic-network"));
    assert(loadInstallation(loaded, &seed, 2) && !strcmp(loaded.ssid, "replacement-network"));
    strcpy(seed.ssid, "third-network");
    assert(loadInstallation(loaded, &seed, 2) && !strcmp(loaded.ssid, "replacement-network"));
    assert(loadInstallation(loaded, &seed, 3) && !strcmp(loaded.ssid, "third-network"));
    assert(slotsStored() == 1);

    // A forced reseed that cannot be written keeps the stored record.
    Preferences::failWriteKey = "inst-";
    strcpy(seed.ssid, "fourth-network");
    assert(loadInstallation(loaded, &seed, 4) && loaded.forcedRevision == 3);
    assert(!strcmp(loaded.ssid, "third-network"));
    Preferences::failWriteKey.clear();
    assert(loadInstallation(loaded) && loaded.forcedRevision == 3);
}

static void legacy_record_migrates_to_a_slot() {
    resetStore();
    settingsNamespaceExists = true;
    settingsPreferences[installationKey] = schema1Blob();
    Installation loaded{};
    assert(loadInstallation(loaded));
    assert(loaded.schema == 2 && loaded.forcedRevision == 7 && !strcmp(loaded.profile, "desk-display"));
    assert(!strcmp(loaded.timezone, "CST6CDT,M3.2.0,M11.1.0") && strlen(loaded.deviceKey) == 64);
    assert(!loaded.zip[0] && loaded.units == 0 && loaded.clock24h == 1 && std::isnan(loaded.latitude));
    assert(loaded.quietStart == 23 * 60 && loaded.callState == uint8_t(CallState::None));
    assert(slotsStored() == 1 && !settingsPreferences.count(installationKey));  // legacy removed after
    Installation again{};
    assert(loadInstallation(again) && same(again, loaded));

    // Without persistence (an update awaiting confirmation) nothing is rewritten.
    resetStore();
    settingsNamespaceExists = true;
    settingsPreferences[installationKey] = schema1Blob();
    assert(loadInstallation(loaded, nullptr, 0, false) && loaded.forcedRevision == 7);
    assert(slotsStored() == 0 && settingsPreferences.count(installationKey));

    // A truncated legacy blob is not a record.
    settingsPreferences[installationKey].pop_back();
    assert(!loadInstallation(loaded));
}

// The bench failure of 1.1.0: rewriting the record in place in a full NVS
// lost both copies. The legacy key is never the write target, so a failed
// write leaves it, and the board keeps running from the migrated copy.
static void failed_migration_write_keeps_legacy() {
    resetStore();
    settingsNamespaceExists = true;
    settingsPreferences[installationKey] = schema1Blob();
    Preferences::failWriteKey = "inst-";
    Preferences::eraseOnFailedWrite = true;
    Installation loaded{};
    assert(loadInstallation(loaded) && loaded.forcedRevision == 7);
    assert(settingsPreferences.count(installationKey) && slotsStored() == 0);
    Preferences::failWriteKey.clear();
    Preferences::eraseOnFailedWrite = false;
    assert(loadInstallation(loaded) && slotsStored() == 1 && !settingsPreferences.count(installationKey));
}

static void raw_schema2_legacy_is_accepted() {  // what 1.1.0 tried to write in place
    resetStore();
    settingsNamespaceExists = true;
    Installation raw = pairedSeed();
    const auto* bytes = reinterpret_cast<const uint8_t*>(&raw);
    settingsPreferences[installationKey] = std::vector<uint8_t>(bytes, bytes + sizeof raw);
    Installation loaded{};
    assert(loadInstallation(loaded) && same(loaded, raw) && slotsStored() == 1);
}

static void saves_alternate_slots_and_drop_the_old_copy_last() {
    resetStore();
    Installation one = standaloneRecord(), loaded{};
    assert(saveInstallation(one));
    assert(settingsPreferences.count(installationSlotA) && !settingsPreferences.count(installationSlotB));
    Installation two = one;
    strcpy(two.zip, "54321");
    assert(saveInstallation(two));
    assert(!settingsPreferences.count(installationSlotA) && settingsPreferences.count(installationSlotB));
    assert(loadInstallation(loaded) && !strcmp(loaded.zip, "54321"));

    // A failed save, even one that loses its target, never touches the good slot.
    Installation three = two;
    strcpy(three.zip, "11111");
    Preferences::failWriteKey = installationSlotA;
    Preferences::eraseOnFailedWrite = true;
    assert(!saveInstallation(three));
    Preferences::failWriteKey.clear();
    Preferences::eraseOnFailedWrite = false;
    assert(loadInstallation(loaded) && !strcmp(loaded.zip, "54321"));

    assert(!saveInstallation(Installation{}));  // an invalid record is refused, not stored
    assert(loadInstallation(loaded) && !strcmp(loaded.zip, "54321"));
}

static void newest_valid_slot_wins() {
    resetStore();
    Installation one = standaloneRecord(), two = standaloneRecord(), loaded{};
    strcpy(two.zip, "54321");
    assert(saveInstallation(one));
    const auto older = settingsPreferences.at(installationSlotA);
    assert(saveInstallation(two));
    settingsPreferences[installationSlotA] = older;  // an interrupted cleanup leaves both
    assert(loadInstallation(loaded) && !strcmp(loaded.zip, "54321"));
    settingsPreferences[installationSlotB].back() ^= 1;  // corrupt the newer copy
    assert(loadInstallation(loaded) && !strcmp(loaded.zip, "12345"));
    settingsPreferences[installationSlotA][8] ^= 1;      // and the older one
    assert(!loadInstallation(loaded));
}

static void future_schema_is_left_alone() {
    resetStore();
    Installation future = standaloneRecord();
    assert(saveInstallation(future));
    auto& blob = settingsPreferences.at(installationSlotA);
    std::vector<uint8_t> payload(blob.begin() + installationBlobHeader, blob.end());
    payload[0] = 3;  // schema 3, from newer firmware
    blob = encodeInstallationBlobForTest(payload, 1);
    Installation seed = pairedSeed(), loaded{};
    assert(!loadInstallation(loaded, &seed, 5));          // a downgrade must not overwrite it
    assert(settingsPreferences.at(installationSlotA) == blob);
}

static void blobs_are_compact() {
    resetStore();
    assert(saveInstallation(standaloneRecord()));
    const size_t standaloneBytes = settingsPreferences.at(installationSlotA).size();
    assert(saveInstallation(pairedSeed()));
    const size_t pairedBytes = settingsPreferences.at(installationSlotB).size();
    printf("installation blob bytes standalone=%zu paired=%zu (was 4700)\n", standaloneBytes, pairedBytes);
    assert(standaloneBytes < 800 && pairedBytes < 800 + strlen(pairedSeed().certificate));
}

static void standalone_record_rules() {
    Installation s = standaloneRecord();
    assert(validInstallation(s) && standalone(s) && !installationTransportAllowed(s, true));
    strcpy(s.zip, "12345-6789");
    assert(validInstallation(s));
    for (const char* bad : {"1234", "ABCDE", "12345-", "123456"}) {
        strcpy(s.zip, bad);
        assert(!validInstallation(s));
    }
    s.zip[0] = '\0';
    assert(!validInstallation(s));
    s.latitude = 40.7f;
    assert(!validInstallation(s));
    s.longitude = -74.0f;
    assert(validInstallation(s));
    s = standaloneRecord();
    strcpy(s.pairing, "desk");
    assert(!validInstallation(s));
    s = standaloneRecord();
    s.units = 2;
    assert(!validInstallation(s));
    s = standaloneRecord();
    s.callState = 4;
    assert(!validInstallation(s));
    for (const char* ok : {"", "K1ABC", "W1AW", "KD2XYZ"}) {
        s = standaloneRecord();
        strcpy(s.callSign, ok);
        assert(validInstallation(s));
    }
    for (const char* bad : {"k1abc", "K1 ABC", "K1", "K1-ABC"}) {
        s = standaloneRecord();
        strcpy(s.callSign, bad);
        assert(!validInstallation(s));
    }
    s = standaloneRecord();
    s.quietStart = s.quietEnd = -1;
    assert(validInstallation(s));
    s.quietStart = -1; s.quietEnd = 360;
    assert(!validInstallation(s));
    s.quietStart = 1440; s.quietEnd = 360;
    assert(!validInstallation(s));
}

int main() {
    schema2_layout_pinned();
    read_only_start_and_seeding();
    seed_is_write_once_unless_forced();
    legacy_record_migrates_to_a_slot();
    failed_migration_write_keeps_legacy();
    raw_schema2_legacy_is_accepted();
    saves_alternate_slots_and_drop_the_old_copy_last();
    newest_valid_slot_wins();
    future_schema_is_left_alone();
    blobs_are_compact();
    standalone_record_rules();
    puts("lcd43b installation: seeding, write-once, two-slot saves, legacy migration without loss passed");
}
