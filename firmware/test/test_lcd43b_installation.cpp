// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
#include "../lcd43b/installation.h"
#include <cassert>
#include <cstdio>

int main() {
    using namespace lcd;
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
    assert(!loadInstallation(loaded, &seed, 4));
    assert(loadInstallation(loaded) && loaded.forcedRevision == 3);
    assert((preferences.at("lcd-sec-counter") == std::vector<uint8_t>{1, 2, 3, 4}));
    puts("lcd43b installation: read-only start, paired seed, force-once, failed write and counter preservation passed");
}
