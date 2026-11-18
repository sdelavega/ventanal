// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
#pragma once

#include "build_config.h"
#include "../common/release_key.h"
#include "installation.h"
#include "transport.h"
#include <WiFi.h>
#ifndef VENTANAL_HOST_TEST
#include <WiFiClientSecure.h>
#include <mbedtls/x509_crt.h>
#endif

namespace lcd {
#ifndef VENTANAL_HOST_TEST
class PinnedLeafClient : public WiFiClientSecure {
public:
    void setPins(const char* pem) {
        pins_ = pem;
        setCACert(pem);
    }

    int connect(const char* host, uint16_t port, int32_t timeout) override {
        if (!pins_ || !WiFiClientSecure::connect(host, port, timeout)) return 0;
        const auto* peer = getPeerCertificate();
        mbedtls_x509_crt pins;
        mbedtls_x509_crt_init(&pins);
        bool matched = false;
        if (peer && mbedtls_x509_crt_parse(&pins,
                reinterpret_cast<const unsigned char*>(pins_), strlen(pins_) + 1) == 0) {
            for (auto* pin = &pins; pin && !matched; pin = pin->next) {
                matched = peer->raw.len == pin->raw.len && pin->raw.len &&
                    memcmp(peer->raw.p, pin->raw.p, pin->raw.len) == 0;
            }
        }
        mbedtls_x509_crt_free(&pins);
        if (!matched) stop();
        return matched;
    }

private:
    const char* pins_ = nullptr;
};
#endif

struct HostRequest {
    WiFiClient plain;
#ifndef VENTANAL_HOST_TEST
    PinnedLeafClient tls;
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
            tls.setPins(installation.certificate);
            tls.setHandshakeTimeout(5);
            if (!http.begin(tls, url)) return false;
#endif
            if (!addPairedHeaders(http, key, installation.pairing, path.c_str())) return false;
        } else {
            if (!VENTANAL_TEST_RELEASE_KEY || strncmp(origin, "http://", 7) != 0 || installation.certificate[0]
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
