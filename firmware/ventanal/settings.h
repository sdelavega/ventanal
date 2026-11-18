// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
#pragma once
#include "../common/installation.h"
#include <cstdint>
#include <cstring>

inline constexpr uint32_t settingsSchema = 1;
inline constexpr char settingsNamespace[] = "ventanal-cfg";
inline constexpr char settingsKey[] = "installation";

// One blob commits the complete installation, including both PEMs during
// rotation. Keep the schema word first so migrations can inspect old blobs.
struct InstallationSettings {
    uint32_t schema = settingsSchema;
    uint32_t forcedRevision = 0;
    int32_t wakeMinutes = 30;
    int32_t quietStart = 1380;
    int32_t quietEnd = 360;
    uint8_t clock24h = 1;
    uint8_t rotation = 1;
    char ssid[33]{};
    char password[65]{};
    char host[256]{};
    char timezone[128] = "UTC0";
    char certificate[4096]{};
    char deviceKey[65]{};
};
static_assert(sizeof(InstallationSettings) == 4668, "NVS installation schema 1 layout");

inline bool validSettings(const InstallationSettings& s) {
    auto terminated = [](const auto& field) { return memchr(field, '\0', sizeof field) != nullptr; };
    return s.schema == settingsSchema && terminated(s.ssid) && s.ssid[0]
        && strcmp(s.ssid, "your-ssid") != 0 && terminated(s.password)
        && strcmp(s.password, "your-password") != 0 && terminated(s.host)
        && ((strncmp(s.host, "http://", 7) == 0 && s.host[7])
            || (strncmp(s.host, "https://", 8) == 0 && s.host[8]))
        && terminated(s.timezone) && s.timezone[0] && terminated(s.certificate)
        && terminated(s.deviceKey) && s.wakeMinutes > 0 && s.wakeMinutes <= 1440
        && s.quietStart >= 0 && s.quietStart < 1440 && s.quietEnd >= 0 && s.quietEnd < 1440
        && s.clock24h <= 1 && (s.rotation == 1 || s.rotation == 3);
}

inline bool migrateSettings(InstallationSettings&, size_t) {
    // Schema 1 is the first stored format. Add old-schema conversions here.
    return false;
}

inline bool loadSettings(InstallationSettings& out, const InstallationSettings* seed = nullptr,
                         uint32_t forceRevision = 0, bool persistMigration = true) {
    return loadInstallationSettings(out, settingsNamespace, settingsKey, seed, forceRevision,
                                    persistMigration, validSettings, migrateSettings);
}

#ifdef VENTANAL_HOST_TEST
// Public fixtures keep the network and wake harness independent of flash.
#include "security_config.h"
inline InstallationSettings installation{
    settingsSchema, 0, VENTANAL_WAKE_MINUTES, VENTANAL_QUIET_START, VENTANAL_QUIET_END,
    VENTANAL_CLOCK_24H, VENTANAL_ROTATION, VENTANAL_WIFI_SSID, VENTANAL_WIFI_PASSWORD,
    VENTANAL_HOST_URL, VENTANAL_TZ, VENTANAL_HOST_CERT_PEM, VENTANAL_DEVICE_KEY
};
inline bool hostSettingsAvailable = true;
inline bool initializeSettings(bool) { return hostSettingsAvailable; }
#else
inline InstallationSettings installation{};
bool initializeSettings(bool persistMigration);
#endif
