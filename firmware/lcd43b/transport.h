// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
#pragma once

#include "manifest.h"
#include "../ventanal/manifest_signature.h"
#include <HTTPClient.h>
#include <nvs.h>
#include <atomic>

namespace lcd {

// Only the refresh worker issues counters. The loop consumes the flash
// notification independently, even if the authenticated request fails.
inline std::atomic<bool> pairingCounterFlash{false};

class PairingCounters {
    struct Record {
        char fingerprint[64];
        uint64_t highWater;
    };

    static constexpr char Namespace[] = "lcd-sec";
    static constexpr char CounterKey[] = "high-water";

    // Distinguish absent records from corrupt or unreadable ones. A
    // same-key upgrade must keep the counter issued by the old firmware.
    static esp_err_t read(const char* space, const char* key, Record& record) {
        nvs_handle_t handle;
        auto status = nvs_open(space, NVS_READONLY, &handle);
        if (status != ESP_OK) return status;
        size_t size = sizeof record;
        status = nvs_get_blob(handle, key, &record, &size);
        nvs_close(handle);
        if (status == ESP_OK && size != sizeof record) return ESP_ERR_NVS_INVALID_LENGTH;
        return status;
    }

public:
    static constexpr uint64_t BlockSize = 64;

    bool next(const std::string& key, uint64_t& counter) {
        if (key == key_ && issued_ < reserved_) {
            counter = ++issued_;
            return true;
        }

        Record record{};
        const auto fingerprint = hmacHex(key, "Ventanal LCD replay counter");
        if (fingerprint.size() != sizeof record.fingerprint) return false;
        auto status = read(Namespace, CounterKey, record);
        if (status == ESP_ERR_NVS_NOT_FOUND)
            status = read("ventanal", "lcd-sec-counter", record);
        if (status != ESP_OK && status != ESP_ERR_NVS_NOT_FOUND) return false;

        const uint64_t base = status == ESP_OK &&
            memcmp(record.fingerprint, fingerprint.data(), sizeof record.fingerprint) == 0
            ? record.highWater : 0;
        if (base == UINT64_MAX) return false;
        memcpy(record.fingerprint, fingerprint.data(), sizeof record.fingerprint);
        record.highWater = UINT64_MAX - base < BlockSize ? UINT64_MAX : base + BlockSize;

        nvs_handle_t handle;
        if (nvs_open(Namespace, NVS_READWRITE, &handle) != ESP_OK) return false;
        const auto written = nvs_set_blob(handle, CounterKey, &record, sizeof record);
        const bool committed = written == ESP_OK && nvs_commit(handle) == ESP_OK;
        nvs_close(handle);
        // NVS may program flash even when the operation eventually fails.
        pairingCounterFlash.store(true);
        if (!committed) return false;

        key_ = key;
        reserved_ = record.highWater;
        issued_ = base + 1;
        counter = issued_;
        return true;
    }

private:
    std::string key_;
    uint64_t issued_ = 0;
    uint64_t reserved_ = 0;
};

inline bool nextPairingCounter(const std::string& key, uint64_t& counter) {
    static PairingCounters counters;
    return counters.next(key, counter);
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
