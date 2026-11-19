// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
#include "../lcd43b/installation.h"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>

using namespace lcd;

// A schema-1 record is the 4700-byte prefix of schema 2; filling every byte
// with a field-specific pattern proves migration keeps them all.
static std::vector<uint8_t> schema1Blob() {
    Installation v1{};
    v1.schema = 1;
    v1.forcedRevision = 7;
    strcpy(v1.ssid, "legacy-network");
    strcpy(v1.password, "legacy-password");
    strcpy(v1.host, "https://legacy.test/");
    strcpy(v1.timezone, "CST6CDT,M3.2.0,M11.1.0");
    strcpy(v1.profile, "legacy-lcd");
    strcpy(v1.pairing, "legacy");
    strcpy(v1.certificate, "legacy PEM");
    memset(v1.deviceKey, 'b', 64);
    const auto* bytes = reinterpret_cast<const uint8_t*>(&v1);
    return std::vector<uint8_t>(bytes, bytes + 4700);
}

static void schema2_size_pinned() {
    static_assert(installationSchema == 2);
    static_assert(offsetof(Installation, deviceKey) + sizeof(Installation{}.deviceKey) == 4700,
                  "schema-1 fields must keep their offsets");
    assert(sizeof(Installation) == 4792);
}

static void migrate_preserves_schema1_bytes() {
    settingsPreferences.clear();
    settingsNamespaceExists = true;
    const auto blob = schema1Blob();
    settingsPreferences[installationKey] = blob;
    Installation loaded{};
    assert(loadInstallation(loaded));
    assert(loaded.schema == 2 && loaded.forcedRevision == 7);
    assert(!strcmp(loaded.ssid, "legacy-network") && !strcmp(loaded.password, "legacy-password"));
    assert(!strcmp(loaded.host, "https://legacy.test/") && !strcmp(loaded.profile, "legacy-lcd"));
    assert(!strcmp(loaded.timezone, "CST6CDT,M3.2.0,M11.1.0") && !strcmp(loaded.pairing, "legacy"));
    assert(!strcmp(loaded.certificate, "legacy PEM") && strlen(loaded.deviceKey) == 64);
    assert(!loaded.zip[0] && loaded.units == 0 && loaded.clock24h == 1);
    assert(loaded.quietStart == 23 * 60 && loaded.quietEnd == 6 * 60);
    assert(std::isnan(loaded.latitude) && std::isnan(loaded.longitude) && !loaded.place[0]);
    assert(!loaded.callSign[0] && loaded.callState == uint8_t(CallState::None) && !loaded.callCheckedAt);
    // Persisted once as a whole schema-2 record; the old prefix is intact.
    const auto& stored = settingsPreferences.at(installationKey);
    assert(stored.size() == sizeof(Installation) && stored[0] == 2);
    assert(std::equal(blob.begin() + 4, blob.end(), stored.begin() + 4));
    // A truncated schema-1 record is not migrated.
    settingsPreferences[installationKey] = std::vector<uint8_t>(blob.begin(), blob.begin() + 4699);
    assert(!loadInstallation(loaded));
}

static Installation standaloneRecord() {
    Installation s{};
    strcpy(s.ssid, "synthetic-network");
    strcpy(s.password, "synthetic-password");
    strcpy(s.zip, "12345");
    return s;
}

static void standalone_record_valid() {
    Installation s = standaloneRecord();
    assert(validInstallation(s) && standalone(s));
    assert(!installationTransportAllowed(s, true));  // no host to talk to
    strcpy(s.zip, "12345-6789");
    assert(validInstallation(s));
    for (const char* bad : {"1234", "ABCDE", "12345-", "123456"}) {
        strcpy(s.zip, bad);
        assert(!validInstallation(s));
    }
    s.zip[0] = '\0';
    assert(!validInstallation(s));                    // no location at all
    s.latitude = 40.7f;
    assert(!validInstallation(s));                    // half a coordinate
    s.longitude = -74.0f;
    assert(validInstallation(s));
    s = standaloneRecord();
    strcpy(s.pairing, "desk");
    assert(!validInstallation(s));                    // pairing without a host
    s = standaloneRecord();
    s.units = 2;
    assert(!validInstallation(s));
    s = standaloneRecord();
    s.callState = 4;
    assert(!validInstallation(s));
}

static void callsign_charset() {
    Installation s = standaloneRecord();
    for (const char* ok : {"", "K1ABC", "W1AW", "KD2XYZ"}) {
        strcpy(s.callSign, ok);
        assert(validInstallation(s));
    }
    for (const char* bad : {"k1abc", "K1 ABC", "K1", "K1-ABC", "ABCDEFGHIJK"}) {
        memset(s.callSign, 0, sizeof s.callSign);
        memcpy(s.callSign, bad, std::min(strlen(bad), sizeof s.callSign));
        assert(!validInstallation(s));
    }
}

static void quiet_pair_rule() {
    Installation s = standaloneRecord();
    s.quietStart = s.quietEnd = -1;
    assert(validInstallation(s));
    s.quietStart = 0; s.quietEnd = 1439;
    assert(validInstallation(s));
    s.quietStart = -1; s.quietEnd = 360;
    assert(!validInstallation(s));
    s.quietStart = 1440; s.quietEnd = 360;
    assert(!validInstallation(s));
    s.quietStart = -2; s.quietEnd = -2;
    assert(!validInstallation(s));
}


int main() {
    Installation loaded{};
    assert(!loadInstallation(loaded));
    assert(!settingsNamespaceExists && settingsPreferences.empty());

    Installation seed{};
    strcpy(seed.ssid, "your-network-name");
    strcpy(seed.host, "http://frame.test");
    assert(!loadInstallation(loaded, &seed, 1));
    assert(!settingsNamespaceExists && settingsPreferences.empty());

    strcpy(seed.ssid, "synthetic-network");
    strcpy(seed.password, "synthetic-password");
    strcpy(seed.host, "https://frame.test/");
    strcpy(seed.timezone, "PST8PDT,M3.2.0,M11.1.0");
    strcpy(seed.profile, "desk-display");
    strcpy(seed.pairing, "desk");
    assert(!loadInstallation(loaded, &seed));  // a partial pairing is unusable
    memset(seed.deviceKey, 'a', 64);
    strcpy(seed.certificate, "synthetic first PEM\nsynthetic second PEM");
    preferences["lcd-sec-counter"] = {1, 2, 3, 4};
    assert(loadInstallation(loaded, &seed));
    assert(installationTransportAllowed(loaded, false));
    Installation plain = loaded;
    strcpy(plain.host, "http://frame.test");
    plain.pairing[0] = plain.certificate[0] = plain.deviceKey[0] = '\0';
    assert(installationTransportAllowed(plain, true));
    assert(!installationTransportAllowed(plain, false));
    assert(!strcmp(loaded.host, seed.host) && !strcmp(loaded.profile, seed.profile));
    assert(!strcmp(loaded.deviceKey, seed.deviceKey) && !strcmp(loaded.pairing, seed.pairing));
    assert(!strcmp(loaded.certificate, seed.certificate));
    assert(!strcmp(loaded.timezone, seed.timezone) && !strcmp(loaded.password, seed.password));

    strcpy(seed.ssid, "replacement-network");
    assert(loadInstallation(loaded, &seed) && !strcmp(loaded.ssid, "synthetic-network"));
    assert(loadInstallation(loaded, nullptr, 99) && !strcmp(loaded.ssid, "synthetic-network"));
    assert(loadInstallation(loaded, &seed, 2) && !strcmp(loaded.ssid, "replacement-network"));
    strcpy(seed.ssid, "third-network");
    assert(loadInstallation(loaded, &seed, 2) && !strcmp(loaded.ssid, "replacement-network"));
    assert(loadInstallation(loaded, &seed, 3) && !strcmp(loaded.ssid, "third-network"));

    Preferences::failWriteKey = installationKey;
    assert(loadInstallation(loaded, &seed, 4) && loaded.forcedRevision == 3);
    assert(loadInstallation(loaded) && loaded.forcedRevision == 3);
    Preferences::failWriteKey.clear();

    settingsPreferences[installationKey] = {1, 0, 0, 0};
    assert(!loadInstallation(loaded));
    assert(loadInstallation(loaded, &seed) && !strcmp(loaded.ssid, "third-network"));

    settingsPreferences[installationKey][0] = 3;  // a future schema is left alone
    assert(!loadInstallation(loaded, &seed, 5));
    assert(settingsPreferences.at(installationKey)[0] == 3);
    assert((preferences.at("lcd-sec-counter") == std::vector<uint8_t>{1, 2, 3, 4}));
    puts("lcd43b installation: read-only start, paired seed, force-once, failed write and counter preservation passed");

    schema2_size_pinned();
    migrate_preserves_schema1_bytes();
    standalone_record_valid();
    callsign_charset();
    quiet_pair_rule();
    puts("lcd43b installation schema 2: size, migration, standalone, call sign and quiet rules passed");
}
