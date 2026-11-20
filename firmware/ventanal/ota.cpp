// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
#include "ota.h"

#include "http.h"
#include "net.h"
#include "security.h"
#include "../common/release.h"
#include "../common/resume/ota_stage.h"
#include "../common/resume/storage.h"
#include <LittleFS.h>
#include <Update.h>
#include <algorithm>
#include <esp_ota_ops.h>
#include <memory>

#ifndef VENTANAL_OTA_GITHUB
#define VENTANAL_OTA_GITHUB 0
#endif
#if VENTANAL_OTA_GITHUB
#include "../common/ota_github_transport.h"
#include "../common/https_get.cpp"
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
constexpr char attemptsKey[] = "ota-attempts";
constexpr uint32_t transferMs = 60000;
constexpr uint32_t idleMs = 10000;
// Budget for a full staged-image hash plus a second read, flash write and
// finalization at the largest supported image size.
constexpr uint32_t installReserveMs = 40000;

bool storeRecord(Preferences& prefs, const OtaRecord& record) {
    if (prefs.putBytes(recordKey, &record, sizeof record) == sizeof record) return true;
    Serial.println("ota error=nvs-write");
    return false;
}

void reportFailure(Preferences& prefs, OtaFailure reason, bool exhausted) {
    OtaRecord record{};
    setOtaFailure(record, reason, exhausted);
    char report[otaReportCapacity];
    formatOtaReport(report, record);
    Serial.printf("ota=%s\n", report);
    storeRecord(prefs, record);
}

bool storeAttempts(Preferences& prefs, const OtaAttemptState& state) {
    if (prefs.putBytes(attemptsKey, &state, sizeof state) == sizeof state) return true;
    Serial.println("ota error=nvs-attempt");
    return false;
}

OtaFailure recordAttempt(Preferences& prefs, OtaAttemptState& state, const char* version,
                         const char* approval, bool deferred = false) {
    // Persist the reservation before writing the spare slot so a reset counts.
    auto next = state;
    const auto failure = updateOtaAttemptState(next, version, approval, deferred);
    if (failure == OtaFailure::Attempts) return failure;
    if (!storeAttempts(prefs, next)) return OtaFailure::Write;
    state = next;
    return failure;
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

uint32_t otaClock() { return millis(); }

template<class StillAllowed>
OtaFailure stageAndInstall(Preferences& prefs, const esp_partition_t& target,
                           const ReleaseInfo& release, const char* url, RangeTransport& transport,
                           uint32_t started, OtaAttemptState& attempts, const char* version,
                           const char* approval, StillAllowed stillAllowed, bool& installed,
                           bool& deferred, void (*feed)()) {
    auto available = attempts;
    const bool newOffer = scopeOtaAttemptState(available, version, approval);
    if (available.attempts.count >= otaAttemptLimit) return OtaFailure::Attempts;
    const auto deferInstall = [&](const char* reason) {
        Serial.printf("ota install=deferred reason=%s\n", reason);
        const auto result = recordAttempt(prefs, attempts, version, approval, true);
        deferred = result == OtaFailure::None;
        return result;
    };
    if (!stillAllowed()) return deferInstall("schedule");
    const auto elapsed = millis() - started;
    if (elapsed >= transferMs) {
        logOtaStage(OtaStage::NoProgress, "budget", 0, release.size, logTransferOutcome);
        return deferInstall("budget");
    }

    const bool flash = partialsOnFlash();
    auto& store = partialStore();
    // Selection copies the active spare to /override.vnf before displaying it.
    const char* spares[] = {"/spare0.vnf", "/spare1.vnf", "/spare2.vnf"};
    auto spare = spareFrameEvictor(LittleFS, spares);
    const auto evict = [&] { return flash && spare(); };
    const auto free = [&] { return &partialStore() == &store ? partialFreeBytes() : uint64_t{0}; };
    const char* reason = nullptr;
    const auto outcome = stageUpdate(transport, release, transferMs - elapsed, store, url, true,
                                     otaClock, uint32_t(time(nullptr)), free, evict, logTransferOutcome,
                                     feed, &reason);
    if (!flash && reason && !strcmp(reason, "space")) partialStoreFull();
    if (&partialStore() != &store || outcome == OtaStage::Progress || outcome == OtaStage::NoProgress) {
        // Staging can span wakes on a marginal link. Only charge install deferrals.
        if (newOffer) {
            if (!storeAttempts(prefs, available)) return OtaFailure::Write;
            attempts = available;
        }
        deferred = true;
        return OtaFailure::None;
    }
    if (outcome == OtaStage::Complete) {
        const bool fits = otaInstallFitsWindow(started, otaClock(), transferMs, installReserveMs);
        if (!fits || !stillAllowed()) return deferInstall(fits ? "schedule" : "budget");
    }
    const auto reservation = recordAttempt(prefs, attempts, version, approval);
    if (reservation != OtaFailure::None) return reservation;
    if (outcome == OtaStage::Failed)
        return reason && (!strcmp(reason, "hash") || !strcmp(reason, "size"))
            ? OtaFailure::Digest : OtaFailure::Download;

    const auto failure = installStaged(store, release, feed, [&](fs::File& file) {
        if (millis() - started >= transferMs || !stillAllowed()) return OtaFailure::Schedule;
        return writeImage(prefs, target, release, started, [&](uint8_t* bytes, size_t count) {
            return file.read(bytes, count) == count ? int(count) : -1;
        }, stillAllowed);
    });
    installed = failure == OtaFailure::None;
    return failure;
}

template<class StillAllowed>
OtaFailure installHost(Preferences& prefs, const FirmwareOffer& offer,
                       OtaAttemptState& attempts, StillAllowed stillAllowed, bool& installed,
                       bool& deferred, void (*feed)()) {
    // An approval scopes the attempt budget even when its metadata or path
    // cannot be trusted. Older hosts omit it, so use a bounded path hint.
    char hint[otaVersionCapacity]{};
    const char* tail = strrchr(offer.path, '-');
    if (tail) {
        ++tail;
        const size_t len = strspn(tail, "0123456789.");
        if (len > 1 && len <= sizeof hint && !strcmp(tail + len - 1, ".bin"))
            memcpy(hint, tail, len - 1);
    }
    const auto reject = [&](OtaFailure failure, const char* reason) {
        logOtaStage(OtaStage::Failed, reason, 0, 0, logStage);
        const auto reservation = recordAttempt(prefs, attempts, hint, offer.approval);
        return reservation == OtaFailure::None ? failure : reservation;
    };
    const auto* target = installSlot();
    if (!target) return reject(OtaFailure::Slot, "slot");
    ReleaseInfo release{};
    if (!checkOffer(offer, *target, release)) return reject(OtaFailure::Release, "release");
    char version[otaVersionCapacity];
    versionString(release, version);
    char path[sizeof offer.path];
    hostFirmwarePath(path, version);
    if (strcmp(path, offer.path)) return reject(OtaFailure::Path, "path");

    const auto url = std::string(installation.host) + path;
    const auto started = otaClock();
    HostRangeTransport transport(url.c_str(), started, transferMs, idleMs);
    return stageAndInstall(prefs, *target, release, url.c_str(), transport, started, attempts,
                           version, offer.approval, stillAllowed, installed, deferred, feed);
}

#if VENTANAL_OTA_GITHUB
// Direct checks are diagnostic only. Use the SDK bundle on every connection
// and keep these anonymous requests separate from paired host transport.
constexpr char latestReleaseUrl[] = "https://api.github.com/repos/sdelavega/ventanal/releases/latest";
constexpr char releaseAssetBase[] = "https://github.com/sdelavega/ventanal/releases/download/";
constexpr char githubCheckKey[] = "ota-gh-check64";

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

template<class StillAllowed>
OtaFailure installGithub(Preferences& prefs, OtaAttemptState& attempts, StillAllowed stillAllowed,
                         bool& installed, bool& deferred, void (*feed)()) {
    const auto* target = installSlot();
    if (!target) return OtaFailure::Slot;
    const uint32_t metadataStarted = millis();
    String listing;
    {
        GithubResponse response;
        if (response.get(latestReleaseUrl) != HTTP_CODE_OK
            || !response.readText(listing, 32768, metadataStarted))
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
    if (!doc["assets"].is<JsonArrayConst>()) return OtaFailure::Release;
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
        return millis() - metadataStarted < transferMs && response.getAsset(url)
            && response.readText(body, limit, metadataStarted);
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
    // Inkplate has no local Updates page. A diagnostic build may approve
    // one explicit version; discovering a newer release grants no approval.
#ifdef VENTANAL_OTA_GITHUB_APPROVED_VERSION
    const bool approved = !strcmp(tag + 1, VENTANAL_OTA_GITHUB_APPROVED_VERSION);
#else
    const bool approved = false;
#endif
    if (!approved) {
        Serial.println("ota github=awaiting-approval");
        return OtaFailure::None;
    }
    const String url = assetBase + ".bin";
    const auto started = otaClock();
    OtaGithubTransport transport(url.c_str(), transferMs, otaClock, nullptr);
    return stageAndInstall(prefs, *target, release, url.c_str(), transport, started, attempts,
                           tag + 1, "", stillAllowed, installed, deferred, feed);
}
#endif
}  // namespace

OtaBoot readOtaBoot(Preferences& prefs, bool persisted) {
    OtaBoot boot;
    const auto* running = esp_ota_get_running_partition();
    boot.pending = slotState(running) == OtaSlotState::Pending;
    if (!persisted) return boot;
    auto& attempts = boot.attemptState.attempts;
    if (prefs.isKey(attemptsKey)) {
        // Older firmware stored only OtaAttempts at this key.
        const auto size = prefs.getBytesLength(attemptsKey);
        const bool loaded = size == sizeof attempts
            ? prefs.getBytes(attemptsKey, &attempts, size) == size
            : size == sizeof boot.attemptState
                && prefs.getBytes(attemptsKey, &boot.attemptState, size) == size;
        if (!loaded || !memchr(attempts.version, '\0', sizeof attempts.version)
            || !memchr(attempts.approval, '\0', sizeof attempts.approval)
            || (attempts.approval[0] && !validOtaApproval(attempts.approval))
            || attempts.count > otaAttemptLimit || boot.attemptState.deferrals > otaFreeDeferrals)
            boot.attemptState = {};
    }
    if (!prefs.isKey(recordKey)
        || prefs.getBytes(recordKey, &boot.record, sizeof boot.record) != sizeof boot.record
        || !validOtaRecord(boot.record)) {
        boot.record = {};
        if (attempts.count >= otaAttemptLimit && strcmp(attempts.version, VENTANAL_FIRMWARE_VERSION)) {
            setOtaFailure(boot.record, OtaFailure::Interrupted, true);
        }
        return boot;
    }
    // A new offer can reset the budget while the old exhausted report is still stored.
    if (boot.record.failure == OtaFailure::Attempts && attempts.count < otaAttemptLimit)
        boot.record = {};
    if (boot.record.result != OtaResult::Installing) return boot;
    const auto* target = esp_partition_find_first(ESP_PARTITION_TYPE_APP,
        static_cast<esp_partition_subtype_t>(boot.record.slot), nullptr);
    boot.matching = target && target == running && matchesRunning(boot.record);
    const auto result = otaBootResult(boot.record, target && target == running,
                                      slotState(target), boot.matching);
    if (result != OtaResult::Installing) {
        boot.rolledBack = result == OtaResult::RolledBack;
        boot.record.result = result;
        if (result == OtaResult::Failed)
            setOtaFailure(boot.record, OtaFailure::Interrupted, attempts.count >= otaAttemptLimit);
        else
            boot.record.failure = OtaFailure::None;
        storeRecord(prefs, boot.record);
    }
    return boot;
}

void acknowledgeOtaReport(Preferences& prefs, OtaBoot& boot) {
    // The host normally removes an approval when it receives the rollback.
    // Keep the result and block another install while that approval is offered.
    char path[sizeof FirmwareOffer::path];
    hostFirmwarePath(path, boot.record.version);
    const bool stillOffered = manifestFirmware.release[0] && !strcmp(path, manifestFirmware.path)
        && !strcmp(boot.attemptState.attempts.approval, manifestFirmware.approval);
    // Keep the last failure until a different approval replaces it.
    if (boot.record.failure == OtaFailure::Attempts) return;
    if (!otaCanAcknowledge(boot.record.result, stillOffered)) return;
    if (!prefs.remove(recordKey)) {
        Serial.println("ota error=nvs-ack");
        return;
    }
    if (boot.record.result == OtaResult::Ok || boot.record.result == OtaResult::RolledBack) {
        if (prefs.isKey(attemptsKey) && !prefs.remove(attemptsKey)) Serial.println("ota error=nvs-attempt-ack");
        boot.attemptState = {};
    }
    boot.record = {};
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
    return fs && fs->address == otaCacheOffset && fs->size == otaCacheSize;
}

bool tryOta(Preferences& prefs, const OtaBoot& boot, bool manifestOk, bool timeSynced,
            int battery, int quietStart, int quietEnd, void (*feed)()) {
    auto allowed = [&] { return otaCanInstall(battery, localMinute(), quietStart, quietEnd); };
    if (boot.pending || (boot.record.result != OtaResult::None && boot.record.failure != OtaFailure::Attempts)
        || !allowed()) return false;
    auto attemptState = boot.attemptState;
#if VENTANAL_OTA_GITHUB
    const int64_t now = time(nullptr);
#else
    (void)timeSynced;
#endif

    OtaFailure failure;
    bool installed = false;
    bool deferred = false;
    if (manifestOk && manifestFirmware.release[0]) {
        failure = installHost(prefs, manifestFirmware, attemptState, allowed, installed, deferred, feed);
#if VENTANAL_OTA_GITHUB
    } else if (otaGithubCheckDue(timeSynced, now, prefs.getLong64(githubCheckKey, 0))) {
        failure = installGithub(prefs, attemptState, allowed, installed, deferred, feed);
        if (!deferred && (failure == OtaFailure::None || failure == OtaFailure::Attempts)
            && prefs.putLong64(githubCheckKey, now) != sizeof now) Serial.println("ota error=nvs-week");
#endif
    } else {
        return false;
    }
    if (failure == OtaFailure::Attempts) return false;
    if (failure != OtaFailure::None) {
        reportFailure(prefs, failure, attemptState.attempts.count >= otaAttemptLimit);
        return true;
    }
    if (!installed) return false;
    Serial.println("ota reboot=installed");
    Serial.flush();
    esp_restart();
    return false;
}
