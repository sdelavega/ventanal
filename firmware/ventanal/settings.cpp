// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
#include "settings.h"

#if defined(VENTANAL_RELEASE_BUILD)
bool initializeSettings() {
    return loadSettings(installation);
}
#else
#include "security_config.h"

// A nonzero revision applies once. Use a new revision for the next forced provision.
#ifndef VENTANAL_FORCE_PROVISION
#define VENTANAL_FORCE_PROVISION 0
#endif

bool initializeSettings() {
    static constexpr InstallationSettings seed{
        settingsSchema, 0, VENTANAL_WAKE_MINUTES, VENTANAL_QUIET_START, VENTANAL_QUIET_END,
        VENTANAL_CLOCK_24H, VENTANAL_ROTATION, VENTANAL_WIFI_SSID, VENTANAL_WIFI_PASSWORD,
        VENTANAL_HOST_URL, VENTANAL_TZ, VENTANAL_HOST_CERT_PEM, VENTANAL_DEVICE_KEY
    };
    return loadSettings(installation, &seed, VENTANAL_FORCE_PROVISION);
}
#endif
