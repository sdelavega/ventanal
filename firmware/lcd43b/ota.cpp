// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
#include "ota.h"

#include "../common/release.h"
#include "host_request.h"
#include "sync.h"
#include <LittleFS.h>
#include <esp_littlefs.h>
#include <algorithm>
#include <esp_crt_bundle.h>
#include <esp_http_client.h>
#include <esp_ota_ops.h>
#include <esp_timer.h>
#include <mbedtls/sha256.h>
#include <memory>
#include <sdkconfig.h>

#if !CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE
#error Ventanal LCD OTA requires bootloader rollback support
#endif
#if !CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
#error Ventanal LCD OTA requires the ESP-IDF certificate bundle
#endif
#if defined(VENTANAL_LCD_OTA_BASE_URL) && (!defined(VENTANAL_LCD_DIAG_OTA) || defined(VENTANAL_RELEASE_BUILD))
#error The firmware endpoint override is only available in LCD diagnostic builds
#endif

// Arduino normally confirms a pending app before setup().
extern "C" bool verifyRollbackLater() { return true; }

namespace lcd {
namespace {
constexpr char journalKey[] = "ota-journal";
constexpr char hostAttemptsKey[] = "ota-host-tries";
constexpr char githubAttemptsKey[] = "ota-gh-tries";
constexpr char checkKey[] = "ota-gh-check";
constexpr char autoFailureKey[] = "ota-gh-fail";
constexpr uint32_t transferMs = 60000;
constexpr uint32_t idleMs = 6000;
constexpr uint64_t healthyBootUs = 150ULL * 1000 * 1000;
esp_timer_handle_t updateDeadline = nullptr;

bool saveJournal(Preferences& prefs, const UpdateJournal& journal) {
    if (prefs.putBytes(journalKey, &journal, sizeof journal) == sizeof journal) return true;
    log("ota error=nvs-journal\n");
    return false;
}

template<class Field>
bool terminated(const Field& field) {
    return memchr(field, '\0', sizeof field) != nullptr;
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

const esp_partition_t* installSlot() {
    const auto* running = esp_ota_get_running_partition();
    const auto* spare = esp_ota_get_next_update_partition(nullptr);
    if (!running || !spare || spare == running || running->size != otaSlotSize
        || spare->size != otaSlotSize || esp_ota_get_app_partition_count() != 2
        || esp_ota_get_boot_partition() != running) return nullptr;
    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(running, &state) != ESP_OK
        || (state != ESP_OTA_IMG_VALID && state != ESP_OTA_IMG_UNDEFINED))
        return nullptr;  // wired upload must have seeded a rollback target
    return spare;
}

bool verifyOffer(const FirmwareOffer& offer, UpdateSource source, ReleaseInfo& info,
                 char (&version)[otaVersionCapacity]) {
    if (source == UpdateSource::None || !terminated(offer.release) || !terminated(offer.signature)
        || !terminated(offer.path) || !terminated(offer.approval)
        || (offer.approval[0] && !validOtaApproval(offer.approval))) return false;
    if (checkRelease(offer.release, strlen(offer.release), offer.signature, BOARD_ID,
                     FIRMWARE_VERSION, otaSlotSize, info) != ReleaseVerdict::Ok) return false;
    snprintf(version, sizeof version, "%u.%u.%u", unsigned(info.major), unsigned(info.minor),
             unsigned(info.patch));
    if (source == UpdateSource::GitHub) return !offer.path[0] && !offer.approval[0];
    char path[sizeof offer.path];
    snprintf(path, sizeof path, "/firmware/%s-%s.bin", BOARD_ID, version);
    return !strcmp(path, offer.path);
}

const char* attemptsKey(UpdateSource source) {
    return source == UpdateSource::Host ? hostAttemptsKey : githubAttemptsKey;
}

bool readAttempts(Preferences& prefs, const char* key, OtaAttempts& attempts) {
    return !prefs.isKey(key) || (prefs.getBytes(key, &attempts, sizeof attempts) == sizeof attempts
        && terminated(attempts.version) && terminated(attempts.approval));
}

OtaFailure reserveAttempt(Preferences& prefs, UpdateSource source, const char* version,
                          const char* approval) {
    const char* key = attemptsKey(source);
    OtaAttempts attempts{};
    if (!readAttempts(prefs, key, attempts)) return OtaFailure::Write;
    if (!reserveOtaAttempt(attempts, version, approval)) return OtaFailure::Attempts;
    if (prefs.putBytes(key, &attempts, sizeof attempts) != sizeof attempts) return OtaFailure::Write;
    return OtaFailure::None;
}

template<class Read, class Complete>
OtaFailure writeImage(const esp_partition_t* spare, const ReleaseInfo& release,
                      uint32_t started, Read read, Complete complete) {
    constexpr size_t blockSize = 4096;
    std::unique_ptr<uint8_t[]> block(new(std::nothrow) uint8_t[blockSize]);
    esp_ota_handle_t writer;
    if (!block || esp_ota_begin(spare, OTA_WITH_SEQUENTIAL_WRITES, &writer) != ESP_OK)
        return OtaFailure::Write;

    mbedtls_sha256_context hash;
    mbedtls_sha256_init(&hash);
    bool hashing = mbedtls_sha256_starts(&hash, 0) == 0;
    size_t written = 0;
    OtaFailure failure = OtaFailure::None;
    while (hashing && written < release.size) {
        const size_t limit = std::min(blockSize, size_t(release.size) - written);
        const int size = read(block.get(), limit);
        if (size <= 0 || size_t(size) > limit || millis() - started >= transferMs) {
            failure = OtaFailure::Download;
            break;
        }
        if (esp_ota_write(writer, block.get(), size) != ESP_OK) {
            failure = OtaFailure::Write;
            break;
        }
        written += size;
        hashing = mbedtls_sha256_update(&hash, block.get(), size) == 0;
    }
    uint8_t digest[32]{};
    hashing = hashing && mbedtls_sha256_finish(&hash, digest) == 0;
    mbedtls_sha256_free(&hash);
    if (failure == OtaFailure::None && (!hashing || !releaseImageMatches(release, written, digest)))
        failure = OtaFailure::Digest;
    if (failure == OtaFailure::None && !complete()) failure = OtaFailure::Download;
    if (failure != OtaFailure::None) {
        esp_ota_abort(writer);
        return failure;
    }
    if (esp_ota_end(writer) != ESP_OK) return OtaFailure::Write;
    return OtaFailure::None;
}

OtaFailure downloadHost(const esp_partition_t* spare, const ReleaseInfo& release, const char* path,
                        Watchdog& watchdog) {
    HostRequest request;
    auto& http = request.http;
    if (!request.begin(installation.host, path, decodeDeviceKey(installation.deviceKey)))
        return OtaFailure::Download;
    http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
    const uint32_t started = millis();
    if (http.GET() != HTTP_CODE_OK || http.getSize() != int(release.size)) return OtaFailure::Download;
    auto* stream = http.getStreamPtr();
    if (!stream) return OtaFailure::Download;
    auto read = [&](uint8_t* block, size_t limit) {
        watchdog.feed();
        const uint32_t readStarted = millis();
        while (millis() - started < transferMs && millis() - readStarted < idleMs) {
            const int available = stream->available();
            if (available > 0) return stream->read(block, std::min(limit, size_t(available)));
            if (!http.connected()) break;
            delay(1);
        }
        return -1;
    };
    auto complete = [&] {
        const uint32_t end = millis();
        while (millis() - started < transferMs && millis() - end < FRAME_CLOSE_GRACE_MS) {
            if (stream->available() > 0) return false;
            if (!http.connected()) return true;
            delay(1);
        }
        return false;
    };
    return writeImage(spare, release, started, read, complete);
}

#ifdef VENTANAL_LCD_OTA_BASE_URL
constexpr char latestUrl[] = VENTANAL_LCD_OTA_BASE_URL "/repos/sdelavega/ventanal/releases/latest";
constexpr char assetBaseUrl[] = VENTANAL_LCD_OTA_BASE_URL "/sdelavega/ventanal/releases/download/";
#else
constexpr char latestUrl[] = "https://api.github.com/repos/sdelavega/ventanal/releases/latest";
constexpr char assetBaseUrl[] = "https://github.com/sdelavega/ventanal/releases/download/";
#endif

String assetBase(const char* version) {
    return String(assetBaseUrl) + "v" + version + "/ventanal-" + BOARD_ID + "-" + version;
}

// These requests carry only the product User-Agent, with no host credentials.
// Redirects are explicit so a public response cannot redirect a board to the LAN.
class GithubResponse {
public:
    esp_http_client_handle_t client = nullptr;
    int64_t length = -1;

    ~GithubResponse() { close(); }

    int get(const char* url) {
        esp_http_client_config_t config{};
        config.url = url;
        config.user_agent = USER_AGENT;
        config.timeout_ms = 5000;
        config.disable_auto_redirect = true;
        if (!strncmp(url, "https://", 8)) config.crt_bundle_attach = esp_crt_bundle_attach;
#ifdef VENTANAL_LCD_OTA_BASE_URL
        else if (strncmp(url, "http://", 7)) {
            log("ota github error=unsupported-url-scheme\n");
            return -1;
        }
#else
        else return -1;
#endif
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
#ifdef VENTANAL_LCD_OTA_BASE_URL
        return false;
#else
        if (status != 301 && status != 302 && status != 303 && status != 307 && status != 308)
            return false;
        if (!location_.startsWith("https://release-assets.githubusercontent.com/")
            && !location_.startsWith("https://objects.githubusercontent.com/")) return false;
        const String redirect = location_;
        close();
        return get(redirect.c_str()) == HTTP_CODE_OK;
#endif
    }

    bool readText(String& out, size_t limit, uint32_t started, Watchdog& watchdog) {
        if (length > int64_t(limit)) return false;
        char block[512];
        while (millis() - started < transferMs) {
            watchdog.feed();
            const int got = esp_http_client_read(client, block, sizeof block);
            if (got < 0 || out.length() + size_t(got) > limit) return false;
            if (got > 0 && !out.concat(block, size_t(got))) return false;
            if (esp_http_client_is_complete_data_received(client)) return out.length() > 0;
            if (!got) return false;
        }
        return false;
    }

private:
    String location_;

    void close() {
        if (client) esp_http_client_cleanup(client);
        client = nullptr;
        location_ = "";
    }

    static esp_err_t onHeader(esp_http_client_event_t* event) {
        if (event->event_id == HTTP_EVENT_ON_HEADER && !strcasecmp(event->header_key, "Location")
            && strlen(event->header_value) <= 4096)
            static_cast<GithubResponse*>(event->user_data)->location_ = event->header_value;
        return ESP_OK;
    }
};

OtaFailure downloadGithub(const esp_partition_t* spare, const ReleaseInfo& release,
                          const char* version, Watchdog& watchdog) {
    GithubResponse response;
    const uint32_t started = millis();
    if (!response.getAsset(assetBase(version) + ".bin") || response.length != release.size)
        return OtaFailure::Download;
    auto read = [&](uint8_t* block, size_t limit) {
        watchdog.feed();
        if (millis() - started >= transferMs) return -1;
        return esp_http_client_read(response.client, reinterpret_cast<char*>(block), limit);
    };
    return writeImage(spare, release, started, read, [&] {
        return esp_http_client_is_complete_data_received(response.client);
    });
}
}  // namespace

bool validateOffer(const FirmwareOffer& input, UpdateSource source, UpdateOffer& out) {
    ReleaseInfo release{};
    char version[otaVersionCapacity]{};
    if (!verifyOffer(input, source, release, version)) {
        out = {};
        return false;
    }
    out.signedOffer = input;
    strcpy(out.version, version);
    return true;
}

bool sameOffer(const UpdateOffer& a, const UpdateOffer& b) {
    return !strcmp(a.version, b.version) && !strcmp(a.signedOffer.approval, b.signedOffer.approval)
        && !strcmp(a.signedOffer.signature, b.signedOffer.signature);
}

bool rolledBackOffer(const UpdateBoot& boot, const UpdateOffer& offer) {
    return boot.journal.record.result == OtaResult::RolledBack
        && !strcmp(boot.journal.record.version, offer.version)
        && !strcmp(boot.journal.approval, offer.signedOffer.approval);
}

bool mountUpdateCache(Preferences& prefs) {
    const auto* cache = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                                ESP_PARTITION_SUBTYPE_DATA_SPIFFS, "littlefs");
    if (!cache || cache->address != otaCacheOffset) return false;
    // OTA cannot replace the partition table. Support installations with
    // the original cache geometry and migrate only after a USB table update.
    constexpr size_t reservedForCoredump = 0x10000;
    if (cache->size != otaCacheSize && cache->size != otaCacheSize - reservedForCoredump) return false;
    constexpr char layoutKey[] = "ota-fs-layout";
    const uint8_t layout = cache->size == otaCacheSize ? 1 : 2;
    if (prefs.getUChar(layoutKey) == layout) return LittleFS.begin(true, "/littlefs", 10, "littlefs");

    // Arduino mounts with grow_on_mount. An old superblock can assert
    // if the physical partition has shrunk, so format the named cache
    // partition before any attempt to mount it with the new geometry.
    if (esp_littlefs_format("littlefs") != ESP_OK
        || !LittleFS.begin(false, "/littlefs", 10, "littlefs")) return false;
    if (prefs.putUChar(layoutKey, layout) == sizeof layout) return true;
    LittleFS.end();
    return false;
}

bool armUpdateDeadline() {
    if (slotState(esp_ota_get_running_partition()) != OtaSlotState::Pending) return false;
    // The timer task runs independently of Arduino setup/loop. Resetting a
    // pending image lets the bootloader mark it aborted and select its predecessor.
    esp_timer_create_args_t timer{};
    timer.callback = [](void*) { esp_restart(); };
    timer.name = "ota-health";
    if (esp_timer_create(&timer, &updateDeadline) != ESP_OK
        || esp_timer_start_once(updateDeadline, healthyBootUs) != ESP_OK)
        esp_restart();
    return true;
}

UpdateBoot readUpdateBoot(Preferences& prefs) {
    UpdateBoot boot{};
    const auto* running = esp_ota_get_running_partition();
    boot.pending = slotState(running) == OtaSlotState::Pending;
    if (!prefs.isKey(journalKey)) return boot;
    auto& journal = boot.journal;
    auto& record = journal.record;
    if (prefs.getBytes(journalKey, &journal, sizeof journal) != sizeof journal
        || !validOtaRecord(record) || !terminated(journal.approval)
        || journal.source > UpdateSource::GitHub) {
        journal = {};
        return boot;
    }
    if (record.result != OtaResult::Installing) return boot;
    const auto* target = esp_partition_find_first(ESP_PARTITION_TYPE_APP,
        static_cast<esp_partition_subtype_t>(record.slot), nullptr);
    boot.matching = target && target == running && !strcmp(record.version, FIRMWARE_VERSION);
    const auto result = otaBootResult(record, target && target == running, slotState(target), boot.matching);
    if (result != OtaResult::Installing) {
        record.result = result;
        if (result == OtaResult::Failed) {
            OtaAttempts attempts{};
            const bool capped = readAttempts(prefs, attemptsKey(journal.source), attempts)
                && !strcmp(attempts.version, record.version) && !strcmp(attempts.approval, journal.approval)
                && attempts.count >= otaAttemptLimit;
            record.failure = capped ? OtaFailure::Attempts : OtaFailure::Interrupted;
        }
        saveJournal(prefs, journal);
    }
    return boot;
}

void acknowledgeUpdate(Preferences& prefs, UpdateBoot& boot, const char* sentReport,
                       const char* sentApproval) {
    if (!sentReport[0]) return;
    char current[otaReportCapacity];
    formatOtaReport(current, boot.journal.record);
    if (strcmp(current, sentReport) || strcmp(boot.journal.approval, sentApproval)) return;
    const auto& journal = boot.journal;
    if ((journal.record.result == OtaResult::Ok || journal.record.result == OtaResult::RolledBack)
        && journal.source != UpdateSource::None) {
        const char* key = attemptsKey(journal.source);
        if (prefs.isKey(key) && !prefs.remove(key)) {
            log("ota error=nvs-attempt-ack\n");
            return;
        }
    }
    if (!prefs.isKey(journalKey) || prefs.remove(journalKey)) boot.journal = {};
}

bool confirmUpdate(Preferences& prefs, UpdateBoot& boot) {
    if (!boot.pending || !boot.matching || esp_ota_mark_app_valid_cancel_rollback() != ESP_OK)
        return false;
    if (updateDeadline) {
        esp_timer_stop(updateDeadline);
        esp_timer_delete(updateDeadline);
        updateDeadline = nullptr;
    }
    boot.pending = false;
    boot.journal.record.result = OtaResult::Ok;
    saveJournal(prefs, boot.journal);
    log("ota confirmed=%s\n", boot.journal.record.version);
    return true;
}

[[noreturn]] void rollbackUpdate() {
    log("ota rollback=unhealthy-boot\n");
    esp_ota_mark_app_invalid_rollback_and_reboot();
    esp_restart();  // A reset with pending verification also triggers rollback.
}

OtaFailure installUpdate(Preferences& prefs, UpdateBoot& boot, const UpdateOffer& offer,
                         UpdateSource source, Watchdog& watchdog) {
    DownloadPower radio;
    const auto* spare = boot.pending ? nullptr : installSlot();
    UpdateJournal journal{};
    journal.source = source;
    auto& record = journal.record;
    ReleaseInfo release{};
    auto failure = OtaFailure::Slot;
    if (spare) {
        failure = OtaFailure::Release;
        if (verifyOffer(offer.signedOffer, source, release, record.version)) {
            strcpy(journal.approval, offer.signedOffer.approval);
            failure = reserveAttempt(prefs, source, record.version, journal.approval);
        }
    }
    if (failure == OtaFailure::None) {
        // Clear stale boot metadata before journaling the target. An interrupted
        // transfer is distinguishable from a newly activated image that rolled back.
        esp_ota_img_states_t oldState;
        if (esp_ota_get_state_partition(spare, &oldState) != ESP_ERR_NOT_FOUND
            && (esp_ota_invalidate_inactive_ota_data_slot() != ESP_OK
                || esp_ota_get_state_partition(spare, &oldState) != ESP_ERR_NOT_FOUND))
            failure = OtaFailure::Slot;
    }
    if (failure == OtaFailure::None) {
        record.result = OtaResult::Installing;
        record.slot = spare->subtype;
        if (!saveJournal(prefs, journal)) failure = OtaFailure::Write;
    }
    if (failure == OtaFailure::None) {
        watchdog.feed();
        failure = source == UpdateSource::Host
            ? downloadHost(spare, release, offer.signedOffer.path, watchdog)
            : downloadGithub(spare, release, record.version, watchdog);
        if (failure == OtaFailure::None && esp_ota_set_boot_partition(spare) != ESP_OK)
            failure = OtaFailure::Write;
    }
    if (failure != OtaFailure::None) {
        record.result = OtaResult::Failed;
        record.failure = failure;
        saveJournal(prefs, journal);
    }
    boot.journal = journal;
    return failure;
}

CheckResult checkGithub(int64_t last, GithubCheck& result, Watchdog& watchdog) {
    result = {};
    if (!connectForUpdates(watchdog)) return CheckResult::Failed;
    DownloadPower radio;
    const int64_t now = time(nullptr);
    if (now < 1704067200) return CheckResult::Failed;
    if (!otaGithubCheckDue(true, now, last)) return CheckResult::NotDue;

    const uint32_t started = millis();
    String listing;
    {
        GithubResponse response;
        if (response.get(latestUrl) != HTTP_CODE_OK || !response.readText(listing, 32768, started, watchdog))
            return CheckResult::Failed;
    }
    JsonDocument filter, doc;
    filter["tag_name"] = true;
    filter["assets"][0]["name"] = true;
    if (deserializeJson(doc, listing, DeserializationOption::Filter(filter))) return CheckResult::Failed;
    listing = String();
    const auto tag = doc["tag_name"].as<JsonString>();
    if (tag.isNull() || tag.size() < 2 || tag.c_str()[0] != 'v'
        || !doc["assets"].is<JsonArrayConst>()) return CheckResult::Failed;
    const char* version = tag.c_str() + 1;
    using namespace ventanal_release_detail;
    Cursor cursor{version, tag.c_str() + tag.size()};
    uint16_t major, minor, patch;
    if (!cursor.version(major, minor, patch) || cursor.next != cursor.end) return CheckResult::Failed;
    const char* running = FIRMWARE_VERSION;
    Cursor current{running, running + strlen(running)};
    uint16_t runningMajor, runningMinor, runningPatch;
    if (!current.version(runningMajor, runningMinor, runningPatch) || current.next != current.end)
        return CheckResult::Failed;
    if (versionNumber(major, minor, patch) <= versionNumber(runningMajor, runningMinor, runningPatch)) {
        result.checked = now;
        return CheckResult::UpToDate;
    }
    const String name = String("ventanal-") + BOARD_ID + "-" + version;
    bool hasMetadata = false, hasSignature = false, hasImage = false;
    for (JsonVariantConst asset : doc["assets"].as<JsonArrayConst>()) {
        const char* file = asset["name"] | "";
        hasMetadata |= name + ".json" == file;
        hasSignature |= name + ".json.sig" == file;
        hasImage |= name + ".bin" == file;
    }
    if (!hasMetadata && !hasSignature && !hasImage) {
        result.checked = now;
        return CheckResult::UpToDate;
    }
    if (!hasMetadata || !hasSignature || !hasImage) return CheckResult::Failed;
    const String base = assetBase(version);
    auto fetch = [&](const String& url, String& body, size_t limit) {
        watchdog.feed();
        GithubResponse response;
        return millis() - started < transferMs && response.getAsset(url)
            && response.readText(body, limit, started, watchdog);
    };
    String metadata, signature;
    if (!fetch(base + ".json", metadata, otaReleaseMetadataLimit)
        || !fetch(base + ".json.sig", signature, 128)
        || strlen(metadata.c_str()) != metadata.length() || strlen(signature.c_str()) != signature.length())
        return CheckResult::Failed;
    signature.trim();
    ReleaseInfo release{};
    const auto verdict = checkRelease(metadata.c_str(), metadata.length(), signature.c_str(), BOARD_ID,
                                      running, otaSlotSize, release);
    if (verdict == ReleaseVerdict::TooOld) {
        result.checked = now;
        return CheckResult::UpToDate;
    }
    if (verdict != ReleaseVerdict::Ok || release.major != major || release.minor != minor
        || release.patch != patch || signature.length() >= sizeof result.offer.signature)
        return CheckResult::Failed;

    strcpy(result.offer.release, metadata.c_str());
    strcpy(result.offer.signature, signature.c_str());
    result.checked = now;
    return CheckResult::Available;
}

int64_t loadGithubCheck(Preferences& prefs, UpdateOffer& offer) {
    GithubCheck check{};
    if (!prefs.isKey(checkKey) || prefs.getBytes(checkKey, &check, sizeof check) != sizeof check)
        return 0;
    validateOffer(check.offer, UpdateSource::GitHub, offer);
    return check.checked >= 1704067200 ? check.checked : 0;
}

bool saveGithubCheck(Preferences& prefs, const GithubCheck& result) {
    // Reset only the direct-install budget. Host attempts are tied to approvals.
    if (prefs.isKey(githubAttemptsKey) && !prefs.remove(githubAttemptsKey)) return false;
    return prefs.putBytes(checkKey, &result, sizeof result) == sizeof result;
}

int64_t loadGithubAutoFailure(Preferences& prefs) {
    return prefs.getLong64(autoFailureKey, 0);
}

bool saveGithubAutoFailure(Preferences& prefs, int64_t attemptedAt) {
    return prefs.putLong64(autoFailureKey, attemptedAt) == sizeof attemptedAt;
}
}  // namespace lcd
