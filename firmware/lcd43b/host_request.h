// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
#pragma once

#include "build_config.h"
#include "installation.h"
#include "transport.h"
#include <WiFi.h>
#ifndef VENTANAL_HOST_TEST
#include <WiFiClientSecure.h>
#endif

namespace lcd {
struct HostRequest {
    WiFiClient plain;
#ifndef VENTANAL_HOST_TEST
    WiFiClientSecure tls;
#endif
    HTTPClient http;

    bool begin(const char* origin, const String& path, const std::string& key) {
        size_t originLength = strlen(origin);
        while (originLength && origin[originLength - 1] == '/') --originLength;
        const auto base = std::string(origin, originLength);
        const String url = String(base.c_str()) + path;
        const bool secured = strncmp(origin, "https://", 8) == 0;
        if (secured) {
            if (!installation.certificate[0] || key.size() != 32) return false;
#ifdef VENTANAL_HOST_TEST
            if (!http.begin(plain, url)) return false;
#else
            // The trust bundle contains the current self-signed leaf and,
            // during rotation, may also contain the previous leaf.
            tls.setCACert(installation.certificate);
            tls.setHandshakeTimeout(5);
            if (!http.begin(tls, url)) return false;
#endif
            if (!addPairedHeaders(http, key, installation.pairing, path.c_str())) return false;
        } else {
            if (strncmp(origin, "http://", 7) != 0 || installation.certificate[0]
                || installation.deviceKey[0] || installation.pairing[0]) return false;
            if (!http.begin(plain, url)) return false;
        }
        // The bounded body readers rely on connection-close framing.
        http.setReuse(false);
        http.setTimeout(10000);
        http.setConnectTimeout(5000);
        http.setUserAgent(USER_AGENT);
        return true;
    }

    ~HostRequest() { http.end(); }
};

}  // namespace lcd
