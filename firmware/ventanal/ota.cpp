// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
#include "ota.h"

#include "http.h"
#include "net.h"
#include "security.h"
#include "../common/release.h"
#include <Update.h>
#include <algorithm>
#include <esp_ota_ops.h>
#include <memory>

#ifndef VENTANAL_OTA_GITHUB
#define VENTANAL_OTA_GITHUB 0
#endif
#if VENTANAL_OTA_GITHUB
#include <ArduinoJson.h>
#include <esp_crt_bundle.h>
#include <esp_http_client.h>
#endif

#if !CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE
#error Ventanal OTA requires bootloader rollback support
#endif

// Arduino normally confirms a pending image before setup() runs.
extern "C" bool verifyRollbackLater() { return true; }

namespace {
constexpr char recordKey[] = "ota-record";
constexpr uint32_t transferMs = 60000;
constexpr uint32_t idleMs = 10000;

bool storeRecord(Preferences& prefs, const OtaRecord& record) {
    if (prefs.putBytes(recordKey, &record, sizeof record) == sizeof record) return true;
    Serial.println("ota error=nvs-write");
    return false;
}

void reportFailure(Preferences& prefs, OtaFailure reason) {
    OtaRecord record{};
    record.result = OtaResult::Failed;
    record.failure = reason;
    char report[otaReportCapacity];
    formatOtaReport(report, record);
    Serial.printf("ota=%s\n", report);
    storeRecord(prefs, record);
}

bool matchesRunning(const OtaRecord& record) {
    return !strcmp(record.version, VENTANAL_FIRMWARE_VERSION);
}

OtaSlotState slotState(const esp_partition_t* partition) {
    esp_ota_img_states_t state;
    if (!partition || esp_ota_get_state_partition(partition, &state) != ESP_OK)
        return OtaSlotState::Other;
    switch (state) {
        case ESP_OTA_IMG_PENDING_VERIFY: return OtaSlotState::Pending;
        case ESP_OTA_IMG_VALID: return OtaSlotState::Valid;
        case ESP_OTA_IMG_INVALID:
        case ESP_OTA_IMG_ABORTED: return OtaSlotState::Rejected;
        default: return OtaSlotState::Other;
    }
}

int localMinute() {
    const time_t now = time(nullptr);
    tm local{};
    if (now < 1704067200 || !localtime_r(&now, &local)) return -1;
    return local.tm_hour * 60 + local.tm_min;
}

void versionString(const ReleaseInfo& release, char (&version)[otaVersionCapacity]) {
    snprintf(version, sizeof version, "%u.%u.%u",
             unsigned(release.major), unsigned(release.minor), unsigned(release.patch));
}

void hostFirmwarePath(char (&path)[sizeof FirmwareOffer::path], const char* version) {
    snprintf(path, sizeof path, "/firmware/%s-%s.bin", VENTANAL_BOARD_ID, version);
}

const esp_partition_t* installSlot() {
    const auto* running = esp_ota_get_running_partition();
    const auto* target = esp_ota_get_next_update_partition(nullptr);
    if (!running || !target || target == running || target->size != otaSlotSize
        || esp_ota_get_boot_partition() != running || esp_ota_get_app_partition_count() != 2)
        return nullptr;
    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(running, &state) != ESP_OK
        || (state != ESP_OTA_IMG_VALID && state != ESP_OTA_IMG_UNDEFINED))
        return nullptr;
    return target;
}

bool checkOffer(const FirmwareOffer& offer, const esp_partition_t& target, ReleaseInfo& release) {
    return checkRelease(offer.release, strlen(offer.release), offer.signature,
                        VENTANAL_BOARD_ID, VENTANAL_FIRMWARE_VERSION,
                        target.size, release) == ReleaseVerdict::Ok;
}

template<class Read, class StillAllowed>
OtaFailure writeImage(Preferences& prefs, const esp_partition_t& target,
                      const ReleaseInfo& release, uint32_t started, Read read,
                      StillAllowed stillAllowed) {
    constexpr size_t blockSize = 4096;
    std::unique_ptr<uint8_t[]> block(new(std::nothrow) uint8_t[blockSize]);
    if (!block || !Update.begin(release.size, U_FLASH)) return OtaFailure::Write;

    mbedtls_sha256_context hash;
    mbedtls_sha256_init(&hash);
    bool hashOk = mbedtls_sha256_starts(&hash, 0) == 0;
    size_t written = 0;
    OtaFailure failure = OtaFailure::None;
    while (hashOk && written < release.size) {
        const size_t limit = std::min(blockSize, size_t(release.size) - written);
        const int got = read(block.get(), limit);
        if (got <= 0 || size_t(got) > limit || millis() - started >= transferMs) {
            failure = OtaFailure::Download;
            break;
        }
        if (Update.write(block.get(), got) != size_t(got)) {
            failure = OtaFailure::Write;
            break;
        }
        hashOk = mbedtls_sha256_update(&hash, block.get(), got) == 0;
        written += got;
    }
    uint8_t digest[32]{};
    hashOk = hashOk && mbedtls_sha256_finish(&hash, digest) == 0;
    mbedtls_sha256_free(&hash);
    if (failure == OtaFailure::None && (!hashOk || !releaseImageMatches(release, written, digest)))
        failure = OtaFailure::Digest;
    if (failure == OtaFailure::None && !stillAllowed()) failure = OtaFailure::Schedule;
    if (failure != OtaFailure::None) {
        Update.abort();
        return failure;
    }

    // Reset any previous state for the spare slot before journaling this attempt.
    // A cut between the journal and activation must not look like an old rollback.
    esp_ota_img_states_t oldState;
    if (esp_ota_get_state_partition(&target, &oldState) != ESP_ERR_NOT_FOUND) {
        if (esp_ota_invalidate_inactive_ota_data_slot() != ESP_OK
            || esp_ota_get_state_partition(&target, &oldState) != ESP_ERR_NOT_FOUND) {
            Update.abort();
            return OtaFailure::Slot;
        }
    }
    OtaRecord record{};
    record.result = OtaResult::Installing;
    record.slot = target.subtype;
    versionString(release, record.version);
    if (!storeRecord(prefs, record)) {
        Update.abort();
        return OtaFailure::Write;
    }
    // end() requires the exact byte count, checks the ESP image and selects
    // the new boot partition. The previous image remains the rollback target.
    return Update.end() ? OtaFailure::None : OtaFailure::Write;
}

template<class StillAllowed>
OtaFailure installHost(Preferences& prefs, const FirmwareOffer& offer, StillAllowed stillAllowed) {
    const auto* target = installSlot();
    if (!target) return OtaFailure::Slot;
    ReleaseInfo release{};
    if (!checkOffer(offer, *target, release)) return OtaFailure::Release;
    char version[otaVersionCapacity];
    versionString(release, version);
    char path[sizeof offer.path];
    hostFirmwarePath(path, version);
    if (strcmp(path, offer.path)) return OtaFailure::Path;

    TransportClient localClient;
    auto& client = requestClient(localClient);
    HTTPClient localHttp;
    auto& http = requestHttp(localHttp);
    configureHttp(http);
    http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
    const String url = String(VENTANAL_HOST_URL) + path;
    if (!beginTransport(http, client, url.c_str())) return OtaFailure::Download;
    const uint32_t started = millis();
    OtaFailure reason = OtaFailure::Download;
    if (http.GET() == HTTP_CODE_OK && http.getSize() == int(release.size)) {
        auto* stream = http.getStreamPtr();
        auto read = [&](uint8_t* block, size_t limit) {
            if (!stream) return -1;
            const uint32_t readStarted = millis();
            while (millis() - started < transferMs && millis() - readStarted < idleMs) {
                const int available = stream->available();
                if (available > 0) return stream->read(block, std::min(limit, size_t(available)));
                if (!http.connected()) break;
                delay(1);
            }
            return -1;
        };
        reason = writeImage(prefs, *target, release, started, read, stillAllowed);
    }
    // Close on failure, including a partially consumed response.
    finishTransport(http, reason == OtaFailure::None);
    return reason;
}

#if VENTANAL_OTA_GITHUB
// Direct checks are diagnostic only. Use the SDK bundle on every connection
// and keep these anonymous requests separate from paired host transport.
constexpr char latestReleaseUrl[] = "https://api.github.com/repos/sdelavega/ventanal/releases/latest";
constexpr char releaseAssetBase[] = "https://github.com/sdelavega/ventanal/releases/download/";
constexpr time_t githubInterval = 7 * 24 * 60 * 60;
constexpr char githubCheckKey[] = "ota-gh-check";

struct GithubResponse {
    String location;
    esp_http_client_handle_t client = nullptr;
    int64_t length = -1;

    ~GithubResponse() { close(); }
    void close() {
        if (client) esp_http_client_cleanup(client);
        client = nullptr;
        location = "";
    }

    static esp_err_t onHeader(esp_http_client_event_t* event) {
        if (event->event_id == HTTP_EVENT_ON_HEADER && !strcasecmp(event->header_key, "Location")
            && strlen(event->header_value) <= 4096)
            static_cast<GithubResponse*>(event->user_data)->location = event->header_value;
        return ESP_OK;
    }

    int get(const char* url) {
        esp_http_client_config_t config{};
        config.url = url;
        config.user_agent = USER_AGENT;
        config.timeout_ms = 5000;
        config.disable_auto_redirect = true;
        config.crt_bundle_attach = esp_crt_bundle_attach;
        config.event_handler = onHeader;
        config.user_data = this;
        client = esp_http_client_init(&config);
        if (!client || esp_http_client_open(client, 0) != ESP_OK) return -1;
        length = esp_http_client_fetch_headers(client);
        return length < 0 ? -1 : esp_http_client_get_status_code(client);
    }

    bool getAsset(const String& url) {
        const int status = get(url.c_str());
        if (status == HTTP_CODE_OK) return true;
        if (status != 301 && status != 302 && status != 303 && status != 307 && status != 308)
            return false;
        if (!location.startsWith("https://release-assets.githubusercontent.com/")
            && !location.startsWith("https://objects.githubusercontent.com/")) return false;
        const String redirect = location;
        close();
        return get(redirect.c_str()) == HTTP_CODE_OK;
    }

    bool readText(String& out, size_t limit, uint32_t started) {
        if (length > int64_t(limit)) return false;
        uint8_t block[512];
        while (millis() - started < transferMs) {
            const int got = esp_http_client_read(client, reinterpret_cast<char*>(block), sizeof block);
            if (got < 0 || out.length() + size_t(got) > limit) return false;
            if (got > 0 && !out.concat(reinterpret_cast<const char*>(block), size_t(got))) return false;
            if (esp_http_client_is_complete_data_received(client)) return out.length() > 0;
            if (!got) return false;
        }
        return false;
    }
};

bool weeklyGithubCheck(Preferences& prefs) {
    const uint32_t now = time(nullptr);
    const uint32_t last = prefs.getUInt(githubCheckKey, 0);
    if (last && (now < last || now - last < githubInterval)) return false;
    return prefs.putUInt(githubCheckKey, now) == sizeof now;
}

template<class StillAllowed>
OtaFailure installGithub(Preferences& prefs, StillAllowed stillAllowed, bool& installed) {
    const auto* target = installSlot();
    if (!target) return OtaFailure::Slot;
    const uint32_t started = millis();
    String listing;
    {
        GithubResponse response;
        if (response.get(latestReleaseUrl) != HTTP_CODE_OK || !response.readText(listing, 32768, started))
            return OtaFailure::Download;
    }
    JsonDocument filter, doc;
    filter["tag_name"] = true;
    filter["assets"][0]["name"] = true;
    if (deserializeJson(doc, listing, DeserializationOption::Filter(filter))) return OtaFailure::Release;
    listing = String();
    const char* tag = doc["tag_name"] | "";
    uint16_t major, minor, patch;
    if (*tag != 'v') return OtaFailure::Release;
    ventanal_release_detail::Cursor cursor{tag + 1, tag + strlen(tag)};
    if (!cursor.version(major, minor, patch) || cursor.next != cursor.end) return OtaFailure::Release;
    const String baseName = String("ventanal-") + VENTANAL_BOARD_ID + "-" + (tag + 1);
    const String assetBase = String(releaseAssetBase) + tag + "/" + baseName;
    bool metadata = false, signature = false, image = false;
    for (JsonVariantConst asset : doc["assets"].as<JsonArrayConst>()) {
        const char* name = asset["name"] | "";
        metadata |= baseName + ".json" == name;
        signature |= baseName + ".json.sig" == name;
        image |= baseName + ".bin" == name;
    }
    if (!metadata || !signature || !image) return OtaFailure::None;

    // A checkout running the latest version needs no asset requests.
    using ventanal_release_detail::versionNumber;
    const char* version = VENTANAL_FIRMWARE_VERSION;
    ventanal_release_detail::Cursor current{version, version + strlen(version)};
    uint16_t runningMajor, runningMinor, runningPatch;
    if (!current.version(runningMajor, runningMinor, runningPatch) || current.next != current.end)
        return OtaFailure::Release;
    if (versionNumber(major, minor, patch) <= versionNumber(runningMajor, runningMinor, runningPatch))
        return OtaFailure::None;

    auto fetchText = [&](const String& url, String& body, size_t limit) {
        GithubResponse response;
        return millis() - started < transferMs && response.getAsset(url)
            && response.readText(body, limit, started);
    };
    String releaseJson, releaseSig;
    if (!fetchText(assetBase + ".json", releaseJson, 2048)
        || !fetchText(assetBase + ".json.sig", releaseSig, 128)) return OtaFailure::Download;
    if (strlen(releaseJson.c_str()) != releaseJson.length() || strlen(releaseSig.c_str()) != releaseSig.length())
        return OtaFailure::Release;
    releaseSig.trim();
    ReleaseInfo release{};
    const auto verdict = checkRelease(releaseJson.c_str(), releaseJson.length(), releaseSig.c_str(),
                                      VENTANAL_BOARD_ID, version, target->size, release);
    if (verdict == ReleaseVerdict::TooOld || verdict == ReleaseVerdict::NotNewer) return OtaFailure::None;
    if (verdict != ReleaseVerdict::Ok || major != release.major || minor != release.minor || patch != release.patch)
        return OtaFailure::Release;
    if (!stillAllowed()) return OtaFailure::Schedule;
    GithubResponse response;
    if (millis() - started >= transferMs || !response.getAsset(assetBase + ".bin")
        || response.length != release.size) return OtaFailure::Download;
    auto read = [&](uint8_t* block, size_t limit) {
        if (millis() - started >= transferMs) return -1;
        return esp_http_client_read(response.client, reinterpret_cast<char*>(block), limit);
    };
    const auto failure = writeImage(prefs, *target, release, started, read, stillAllowed);
    installed = failure == OtaFailure::None;
    return failure;
}
#endif
}  // namespace

OtaBoot readOtaBoot(Preferences& prefs, bool persisted) {
    OtaBoot boot;
    const auto* running = esp_ota_get_running_partition();
    boot.pending = slotState(running) == OtaSlotState::Pending;
    if (!persisted || prefs.getBytes(recordKey, &boot.record, sizeof boot.record) != sizeof boot.record
        || !memchr(boot.record.version, '\0', sizeof boot.record.version)) {
        boot.record = {};
        return boot;
    }
    if (boot.record.result != OtaResult::Installing) return boot;
    const auto* target = esp_partition_find_first(ESP_PARTITION_TYPE_APP,
        static_cast<esp_partition_subtype_t>(boot.record.slot), nullptr);
    boot.matching = target && target == running && matchesRunning(boot.record);
    const auto result = otaBootResult(boot.record, target && target == running,
                                      slotState(target), boot.matching);
    if (result != OtaResult::Installing) {
        boot.rolledBack = result == OtaResult::RolledBack;
        boot.record.result = result;
        boot.record.failure = result == OtaResult::Failed ? OtaFailure::Interrupted : OtaFailure::None;
        storeRecord(prefs, boot.record);
    }
    return boot;
}

void acknowledgeOtaReport(Preferences& prefs, OtaBoot& boot) {
    // The host normally removes an approval when it receives the rollback.
    // Keep the result and block another install while that approval is offered.
    char path[sizeof FirmwareOffer::path];
    hostFirmwarePath(path, boot.record.version);
    const bool stillOffered = manifestFirmware.release[0] && !strcmp(path, manifestFirmware.path);
    if (!otaCanAcknowledge(boot.record.result, stillOffered)) return;
    if (prefs.remove(recordKey)) boot.record = {};
    else Serial.println("ota error=nvs-ack");
}

bool confirmOtaBoot(Preferences& prefs, OtaBoot& boot, bool manifestOk, bool screenDrawn) {
    if (!otaCanConfirm(boot.pending, boot.matching, manifestOk, screenDrawn)
        || esp_ota_mark_app_valid_cancel_rollback() != ESP_OK) return false;
    boot.pending = false;
    boot.record.result = OtaResult::Ok;
    storeRecord(prefs, boot.record);
    Serial.printf("ota confirmed=%s\n", boot.record.version);
    return true;
}

void rollbackOtaBoot() {
    Serial.println("ota rollback=unhealthy-boot");
    Serial.flush();
    esp_ota_mark_app_invalid_rollback_and_reboot();
    // A reset of an unconfirmed app also invokes the bootloader's rollback.
    esp_restart();
}

bool otaCacheLayout() {
    const auto* fs = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                              ESP_PARTITION_SUBTYPE_DATA_SPIFFS, "littlefs");
    return fs && fs->address == 0x820000 && fs->size == 0x17d0000;
}

void tryOta(Preferences& prefs, const OtaBoot& boot, bool manifestOk,
            int battery, int quietStart, int quietEnd) {
    auto allowed = [&] { return otaCanInstall(battery, localMinute(), quietStart, quietEnd); };
    if (boot.pending || boot.record.result != OtaResult::None || !allowed()) return;

    OtaFailure failure;
    bool installed=false;
    if (manifestOk && manifestFirmware.release[0]) {
        failure = installHost(prefs, manifestFirmware, allowed);
        installed = failure == OtaFailure::None;
#if VENTANAL_OTA_GITHUB
    } else if (weeklyGithubCheck(prefs)) {
        failure = installGithub(prefs, allowed, installed);
#endif
    } else {
        return;
    }
    if (failure != OtaFailure::None) {
        reportFailure(prefs, failure);
        return;
    }
    if (!installed) return;
    Serial.println("ota reboot=installed");
    Serial.flush();
    esp_restart();
}
