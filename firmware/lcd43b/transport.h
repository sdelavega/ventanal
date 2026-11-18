// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
#pragma once

#include "manifest.h"
#include "../ventanal/manifest_signature.h"
#include <HTTPClient.h>
#include <Preferences.h>

namespace lcd {

inline bool nextPairingCounter(const std::string& key, uint64_t& value) {
    struct CounterRecord {
        char fingerprint[64];
        uint64_t value;
    } record{};
    const auto fingerprint = hmacHex(key, "Ventanal LCD replay counter");
    if (fingerprint.size() != sizeof record.fingerprint) return false;
    Preferences prefs;
    if (!prefs.begin("ventanal", false)) return false;
    bool ok = !prefs.isKey("lcd-sec-counter")
        || prefs.getBytes("lcd-sec-counter", &record, sizeof record) == sizeof record;
    if (ok && memcmp(record.fingerprint, fingerprint.data(), sizeof record.fingerprint)) {
        // Re-pairing issues a new key and resets the server's counter. A pin
        // change alone retains the same counter.
        memcpy(record.fingerprint, fingerprint.data(), sizeof record.fingerprint);
        record.value = 0;
    }
    if (ok && record.value != UINT64_MAX) {
        value = ++record.value;
        ok = prefs.putBytes("lcd-sec-counter", &record, sizeof record) == sizeof record;
    } else {
        ok = false;
    }
    prefs.end();
    return ok;
}

inline bool addPairedHeaders(HTTPClient& http, const std::string& key,
                             const char* pairingId, const char* path) {
    if (key.size() != 32 || !pairingId[0] || strlen(pairingId) > 32 || path[0] != '/')
        return false;
    for (auto p = pairingId; *p; ++p)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '-'))
            return false;
    uint64_t counter;
    if (!nextPairingCounter(key, counter)) return false;
    const auto proof = requestProof(key, "GET", path, counter);
    if (proof.size() != 64) return false;
    http.addHeader("Authorization", (std::string("Ventanal ") + proof).c_str());
    http.addHeader("X-Ventanal-Counter", std::to_string(counter).c_str());
    http.addHeader("X-Ventanal-Device", pairingId);
    return true;
}

inline bool parseSignedDeviceManifest(const char* body, const std::string& key,
                                      DeviceManifest& out) {
    JsonDocument doc;
    return !deserializeJson(doc, body)
        && verifyManifestSignature(doc, key)
        && parseDeviceManifest(doc.as<JsonVariantConst>(), out);
}

}  // namespace lcd
