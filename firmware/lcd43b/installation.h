// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
#pragma once

#include "../common/call_state.h"
#include "../common/installation.h"
#include <cmath>
#include <cstdint>
#include <cstring>

namespace lcd {
inline constexpr uint32_t installationSchema = 2;
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
    // Schema 2 appends the standalone fields; everything above keeps the
    // schema-1 offsets so a stored schema-1 blob is an exact prefix.
    char zip[11]{};                  // "12345" or "12345-6789"; empty when lat/lon are set
    uint8_t units = 0;               // 0 imperial, 1 metric
    uint8_t clock24h = 1;
    int16_t quietStart = 23 * 60;    // minutes of the local day; both -1 = off
    int16_t quietEnd = 6 * 60;
    float latitude = NAN;            // resolved from the ZIP at setup
    float longitude = NAN;
    char place[48]{};                // "City, ST" for the screens
    char callSign[11]{};             // optional US amateur call sign
    uint8_t callState = 0;           // CallState
    uint32_t callCheckedAt = 0;      // epoch seconds of the last FCC answer
};
inline constexpr size_t installationSchema1Bytes = 4700;
static_assert(offsetof(Installation, zip) == installationSchema1Bytes,
              "LCD NVS installation: schema-1 fields must keep their layout");
static_assert(sizeof(Installation) == 4792, "LCD NVS installation schema 2 layout");

// Standalone mode: no host configured, the board fetches and draws itself.
inline bool standalone(const Installation& config) { return !config.host[0]; }

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
        || !terminated(config.pairing) || !terminated(config.certificate) || !terminated(config.deviceKey)
        || !terminated(config.zip) || !terminated(config.place) || !terminated(config.callSign)
        || config.units > 1 || config.clock24h > 1 || config.callState >= callStateCount)
        return false;
    const bool quietOff = config.quietStart == -1 && config.quietEnd == -1;
    auto minute = [](int16_t m) { return m >= 0 && m < 24 * 60; };
    if (!quietOff && !(minute(config.quietStart) && minute(config.quietEnd))) return false;
    const size_t call = strlen(config.callSign);
    if (call && (call < 3 || strspn(config.callSign, "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789") != call))
        return false;
    const size_t zip = strlen(config.zip);
    const bool zipOk = (zip == 5 || (zip == 10 && config.zip[5] == '-'))
        && strspn(config.zip, "0123456789") == 5
        && (zip == 5 || strspn(config.zip + 6, "0123456789") == 4);
    if (zip && !zipOk) return false;
    const bool hasLat = std::isfinite(config.latitude), hasLon = std::isfinite(config.longitude);
    if (hasLat != hasLon) return false;
    if (hasLat && (std::fabs(config.latitude) > 90.f || std::fabs(config.longitude) > 180.f)) return false;
    if (standalone(config))
        return !config.pairing[0] && !config.certificate[0] && !config.deviceKey[0] && (zip || hasLat);
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

inline bool installationTransportAllowed(const Installation& config, bool bench) {
    // Host transport only; a standalone record has no host to talk to.
    return validInstallation(config) && !standalone(config)
        && (bench || !strncmp(config.host, "https://", 8));
}

// The loader read `length` stored bytes over a default record, so a whole
// schema-1 blob already sits in place with the schema-2 fields at their
// defaults; only the schema number changes.
inline bool migrateInstallation(Installation& config, size_t length) {
    if (config.schema != 1 || length != installationSchema1Bytes) return false;
    config.schema = installationSchema;
    return true;
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
