// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
#pragma once

#include "../common/installation.h"
#include <cstdint>
#include <cstring>

namespace lcd {
inline constexpr uint32_t installationSchema = 1;
inline constexpr char installationNamespace[] = "ventanal-cfg";
inline constexpr char installationKey[] = "installation";

struct Installation {
    uint32_t schema = installationSchema;
    uint32_t forcedRevision = 0;
    char ssid[33]{};
    char password[65]{};
    char host[256]{};
    char timezone[128] = "UTC0";
    char profile[16] = "lcd43b";
    char pairing[33]{};
    char certificate[4096]{};
    char deviceKey[65]{};
};
static_assert(sizeof(Installation) == 4700, "LCD NVS installation schema 1 layout");

inline bool validInstallation(const Installation& config) {
    auto terminated = [](const auto& field) { return memchr(field, '\0', sizeof field) != nullptr; };
    auto slug = [&](const auto& field) {
        return terminated(field) && field[0]
            && strspn(field, "abcdefghijklmnopqrstuvwxyz0123456789-") == strlen(field);
    };
    if (config.schema != installationSchema || !terminated(config.ssid) || !config.ssid[0]
        || !strcmp(config.ssid, "your-network-name") || !strcmp(config.ssid, "your-ssid")
        || !terminated(config.password) || !strcmp(config.password, "your-network-password")
        || !strcmp(config.password, "your-password") || !terminated(config.host)
        || !terminated(config.timezone) || !config.timezone[0] || !slug(config.profile)
        || !terminated(config.pairing) || !terminated(config.certificate) || !terminated(config.deviceKey))
        return false;
    const bool tls = !strncmp(config.host, "https://", 8);
    if (!tls && strncmp(config.host, "http://", 7)) return false;
    const char* authority = config.host + (tls ? 8 : 7);
    const size_t length = strcspn(authority, "/");
    if (!length || strpbrk(authority, "?#@ \t\r\n")
        || strspn(authority + length, "/") != strlen(authority + length)) return false;
    if (!tls) return !config.pairing[0] && !config.certificate[0] && !config.deviceKey[0];
    return slug(config.pairing) && config.certificate[0] && strlen(config.deviceKey) == 64
        && strspn(config.deviceKey, "0123456789abcdef") == 64;
}

inline bool migrateInstallation(Installation&, size_t) {
    return false;  // Schema 1 is the first stored LCD format.
}

inline bool loadInstallation(Installation& out, const Installation* seed = nullptr,
                             uint32_t forceRevision = 0, bool persistMigration = true) {
    return loadInstallationSettings(out, installationNamespace, installationKey, seed,
                                    forceRevision, persistMigration, validInstallation,
                                    migrateInstallation);
}

#ifdef VENTANAL_HOST_TEST
// The network harness always uses the public sample's timezone and Wi-Fi.
inline Installation installation{installationSchema, 0, "your-network-name",
    "your-network-password", "", "EST5EDT,M3.2.0,M11.1.0"};
#else
inline Installation installation{};
bool initializeInstallation(bool pendingUpdate);
#endif
}  // namespace lcd
