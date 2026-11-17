// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
#pragma once

// Placeholder configuration for the Waveshare ESP32-S3-Touch-LCD-4.3B.
//
// The sketch has NO local private-config fallback: point the build flag
//   -DVENTANAL_LCD_CONFIG="<absolute path>"
// at a private header kept outside the repository (for example under
// .superpowers/, which is git-ignored) and fill in your own values there.
// Never commit, print or log the private copy.

#define VENTANAL_WIFI_SSID "your-network-name"
#define VENTANAL_WIFI_PASSWORD "your-network-password"
// Local test server on this Mac, e.g. http://192.168.0.10:18780
#define VENTANAL_HOST_URL "http://192.168.0.10:18780"
// Per-device profile published at <output>/devices/<device id>/manifest.json.
#ifndef VENTANAL_DEVICE_ID
#define VENTANAL_DEVICE_ID "lcd43b"
#endif
// POSIX TZ string for local time (quiet hours and serial logs).
#ifndef VENTANAL_TZ
#define VENTANAL_TZ "MST7MDT,M3.2.0,M11.1.0"
#endif