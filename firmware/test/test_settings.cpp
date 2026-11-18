// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
#include "../ventanal/settings.h"
#include <cassert>
#include <cstdio>

int main() {
    auto seed = installation;
    strcpy(seed.ssid, "test network");
    seed.password[0] = '\0';
    strcpy(seed.certificate, "first PEM\nsecond PEM\n");

    InstallationSettings loaded;
    assert(!loadSettings(loaded) && !settingsNamespaceExists);
    assert(!loadSettings(loaded, &installation, 1) && !settingsNamespaceExists);
    assert(loadSettings(loaded, &seed));
    assert(!strcmp(loaded.ssid, seed.ssid));
    assert(!strcmp(loaded.certificate, seed.certificate));
    assert(preferences.empty()); // Pairing counters and other wake state are separate.

    Preferences owner;
    assert(owner.begin(settingsNamespace));
    strcpy(loaded.ssid, "owner change");
    assert(owner.putBytes(settingsKey, &loaded, sizeof loaded) == sizeof loaded);
    Preferences::failWriteKey = settingsKey;
    assert(loadSettings(loaded, &seed)); // Proves the current record needs no write.
    assert(!strcmp(loaded.ssid, "owner change"));
    assert(loadSettings(loaded, nullptr, 1)); // Force cannot seed a generic image.
    assert(!strcmp(loaded.ssid, "owner change"));
    assert(loadSettings(loaded, &seed, 1)); // Failed force leaves the old settings usable.
    assert(!strcmp(loaded.ssid, "owner change") && loaded.forcedRevision == 0);
    Preferences::failWriteKey.clear();
    assert(loadSettings(loaded, &seed, 1));
    assert(!strcmp(loaded.ssid, seed.ssid));
    strcpy(loaded.ssid, "second owner change");
    assert(owner.putBytes(settingsKey, &loaded, sizeof loaded) == sizeof loaded);
    assert(loadSettings(loaded, &seed, 1));
    assert(!strcmp(loaded.ssid, "second owner change"));
    assert(loadSettings(loaded, &seed, 2));
    assert(!strcmp(loaded.ssid, seed.ssid));

    strcpy(loaded.host, "invalid host");
    assert(owner.putBytes(settingsKey, &loaded, sizeof loaded) == sizeof loaded);
    assert(!loadSettings(loaded));
    assert(loadSettings(loaded, &seed)); // Private builds recover a corrupt current record.
    assert(!strcmp(loaded.host, seed.host));

    auto legacy = seed;
    legacy.schema = settingsSchema - 1;
    legacy.forcedRevision = 42;
    assert(owner.putBytes(settingsKey, &legacy, sizeof legacy) == sizeof legacy);
    assert(!loadSettings(loaded));
    auto migrate = +[](InstallationSettings& value, size_t length) {
        if (value.schema != settingsSchema - 1 || length != sizeof value) return false;
        value.schema = settingsSchema;
        return true;
    };
    auto loadMigrated = [&](bool persist) {
        return loadInstallationSettings<InstallationSettings>(
            loaded, settingsNamespace, settingsKey, nullptr, 0, persist, validSettings, migrate);
    };
    assert(loadMigrated(false)); // An unconfirmed image leaves the rollback copy intact.
    assert(loaded.schema == settingsSchema && loaded.forcedRevision == 42);
    assert(owner.getBytes(settingsKey, &legacy, sizeof legacy) == sizeof legacy);
    assert(legacy.schema == settingsSchema - 1);
    Preferences::failWriteKey = settingsKey;
    assert(loadMigrated(true));
    assert(!loadSettings(loaded));
    Preferences::failWriteKey.clear();
    assert(loadMigrated(true));
    assert(loadSettings(loaded) && loaded.forcedRevision == 42);
    assert(!strcmp(loaded.certificate, seed.certificate));

    const uint32_t newSchema = settingsSchema + 1;
    assert(owner.putBytes(settingsKey, &newSchema, sizeof newSchema) == sizeof newSchema);
    assert(!loadSettings(loaded, &seed));
    assert(owner.getBytesLength(settingsKey) == sizeof newSchema);
    uint32_t stored = 0;
    assert(owner.getBytes(settingsKey, &stored, sizeof stored) == sizeof stored && stored == newSchema);

    printf("Installation settings: provisioning policy passed (%zu-byte NVS payload)\n", sizeof loaded);
}
