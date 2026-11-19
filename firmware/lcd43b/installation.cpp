// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
#include "installation.h"
#include "log.h"
#include "../common/release_key.h"

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
    if (!loadInstallation(installation, &seed, VENTANAL_FORCE_PROVISION, !pendingUpdate)) return false;
#else
    if (!loadInstallation(installation, nullptr, 0, !pendingUpdate)) return false;
#endif
#ifdef VENTANAL_LCD_DIAG_STANDALONE_SAMPLE
    // Diagnostic, RAM only: run standalone against the public sample ZIP
    // (90210) with the stored Wi-Fi. Nothing is saved; a normal build boots
    // with the stored record unchanged.
    installation.host[0] = installation.pairing[0] = installation.certificate[0] = installation.deviceKey[0] = '\0';
    strlcpy(installation.zip, "90210", sizeof installation.zip);
    strlcpy(installation.place, "Beverly Hills, CA", sizeof installation.place);
    strlcpy(installation.timezone, "PST8PDT,M3.2.0,M11.1.0", sizeof installation.timezone);
    installation.latitude = 34.0901f;
    installation.longitude = -118.4065f;
    if (!installation.callSign[0]) strlcpy(installation.callSign, "N0CALL", sizeof installation.callSign);  // shows solar
    log("diag standalone-sample ram-only=1\n");
#endif
    // A standalone board needs no host transport: it fetches its own data.
    return standalone(installation) ? validInstallation(installation)
                                    : installationTransportAllowed(installation, VENTANAL_TEST_RELEASE_KEY);
}
}  // namespace lcd
