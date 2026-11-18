// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
#pragma once

#ifdef VENTANAL_LCD_CONFIG
#include VENTANAL_LCD_CONFIG
#else
#include "config.example.h"
#endif
// An optional second private header can supply or override pairing values
// without changing the normal Wi-Fi configuration.
#ifdef VENTANAL_LCD_SECURITY_CONFIG
#include VENTANAL_LCD_SECURITY_CONFIG
#endif

#ifndef VENTANAL_WIFI_SSID
#define VENTANAL_WIFI_SSID "your-network-name"
#endif
#ifndef VENTANAL_WIFI_PASSWORD
#define VENTANAL_WIFI_PASSWORD "your-network-password"
#endif
#ifndef VENTANAL_HOST_URL
#define VENTANAL_HOST_URL "http://192.168.0.10:18780"
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
