// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
#pragma once
#include "net.h"
#include <HTTPClient.h>

inline constexpr char USER_AGENT[] = "Ventanal/" VENTANAL_FIRMWARE_VERSION;

inline void configureHttp(HTTPClient& http) {
    http.setConnectTimeout(5000);
    http.setTimeout(15000);
    http.setUserAgent(USER_AGENT);
}
