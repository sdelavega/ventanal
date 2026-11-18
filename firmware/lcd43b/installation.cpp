// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
#include "installation.h"

#if !defined(VENTANAL_RELEASE_BUILD) && defined(VENTANAL_LCD_CONFIG)
#include VENTANAL_LCD_CONFIG
// An optional second private header can supply or override pairing values
// without changing the normal Wi-Fi configuration.
#ifdef VENTANAL_LCD_SECURITY_CONFIG
#include VENTANAL_LCD_SECURITY_CONFIG
#endif

#ifndef VENTANAL_WIFI_SSID
#define VENTANAL_WIFI_SSID ""
#endif
#ifndef VENTANAL_WIFI_PASSWORD
#define VENTANAL_WIFI_PASSWORD ""
#endif
#ifndef VENTANAL_HOST_URL
#define VENTANAL_HOST_URL ""
#endif
#ifndef VENTANAL_DEVICE_ID
#define VENTANAL_DEVICE_ID "lcd43b"
#endif
#ifndef VENTANAL_TZ
#define VENTANAL_TZ "EST5EDT,M3.2.0,M11.1.0"
#endif
#ifndef VENTANAL_PAIRING_ID
#define VENTANAL_PAIRING_ID ""
#endif
#ifndef VENTANAL_HOST_CERT_PEM
#define VENTANAL_HOST_CERT_PEM ""
#endif
#ifndef VENTANAL_DEVICE_KEY
#define VENTANAL_DEVICE_KEY ""
#endif

#ifndef VENTANAL_FORCE_PROVISION
#define VENTANAL_FORCE_PROVISION 0
#endif
#endif

namespace lcd {
bool initializeInstallation(bool pendingUpdate) {
#if !defined(VENTANAL_RELEASE_BUILD) && defined(VENTANAL_LCD_CONFIG)
    static constexpr Installation seed{
        installationSchema, 0, VENTANAL_WIFI_SSID, VENTANAL_WIFI_PASSWORD,
        VENTANAL_HOST_URL, VENTANAL_TZ, VENTANAL_DEVICE_ID, VENTANAL_PAIRING_ID,
        VENTANAL_HOST_CERT_PEM, VENTANAL_DEVICE_KEY
    };
    return loadInstallation(installation, &seed, VENTANAL_FORCE_PROVISION, !pendingUpdate);
#else
    return loadInstallation(installation, nullptr, 0, !pendingUpdate);
#endif
}
}  // namespace lcd
