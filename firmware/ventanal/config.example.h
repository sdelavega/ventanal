// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
#pragma once
#define VENTANAL_WIFI_SSID "your-ssid"
#define VENTANAL_WIFI_PASSWORD "your-password"
#define VENTANAL_HOST_URL "http://192.168.0.10:8765"
#define VENTANAL_TZ "EST5EDT,M3.2.0,M11.1.0"
#define VENTANAL_WAKE_MINUTES 30
#define VENTANAL_QUIET_START 1380
#define VENTANAL_QUIET_END 360
#define VENTANAL_CLOCK_24H 1
#define VENTANAL_ROTATION 1

// Optional transport security. Copy values from Settings > Security > Reveal.
// Define BOTH values and change VENTANAL_HOST_URL to https://192.168.0.10:8766.
// The complete self-signed PEM is the sole TLS trust anchor; hostname and
// validity are verified. A partial/invalid configuration fails closed.
// #define VENTANAL_HOST_CERT_PEM R"PEM(-----BEGIN CERTIFICATE-----
// ...
// -----END CERTIFICATE-----
// )PEM"
// #define VENTANAL_DEVICE_KEY "64 lowercase hex characters"
