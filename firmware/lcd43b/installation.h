// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
#pragma once

#include "../common/call_state.h"
#include <Preferences.h>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

namespace lcd {
inline constexpr uint32_t installationSchema = 2;
inline constexpr char installationNamespace[] = "ventanal-cfg";
inline constexpr char installationKey[] = "installation";

struct Installation {
    uint32_t schema = installationSchema;
    uint32_t forcedRevision = 0;
    char ssid[33]{};
    char password[65]{};
    char host[256]{};
    char timezone[128] = "UTC0";
    char profile[16] = "lcd43b";
    char pairing[33]{};
    char certificate[4096]{};
    char deviceKey[65]{};
    // Schema 2 appends the standalone fields; everything above keeps the
    // schema-1 offsets so a stored schema-1 blob is an exact prefix.
    char zip[11]{};                  // "12345" or "12345-6789"; empty when lat/lon are set
    uint8_t units = 0;               // 0 imperial, 1 metric
    uint8_t clock24h = 1;
    int16_t quietStart = 23 * 60;    // minutes of the local day; both -1 = off
    int16_t quietEnd = 6 * 60;
    float latitude = NAN;            // resolved from the ZIP at setup
    float longitude = NAN;
    char place[48]{};                // "City, ST" for the screens
    char callSign[11]{};             // optional US amateur call sign
    uint8_t callState = 0;           // CallState
    uint32_t callCheckedAt = 0;      // epoch seconds of the last FCC answer
};
inline constexpr size_t installationSchema1Bytes = 4700;
static_assert(offsetof(Installation, zip) == installationSchema1Bytes,
              "LCD NVS installation: schema-1 fields must keep their layout");
static_assert(sizeof(Installation) == 4792, "LCD NVS installation schema 2 layout");

// Standalone mode: no host configured, the board fetches and draws itself.
inline bool standalone(const Installation& config) { return !config.host[0]; }

inline bool validInstallation(const Installation& config) {
    auto terminated = [](const auto& field) { return memchr(field, '\0', sizeof field) != nullptr; };
    auto slug = [&](const auto& field) {
        return terminated(field) && field[0]
            && strspn(field, "abcdefghijklmnopqrstuvwxyz0123456789-") == strlen(field);
    };
    if (config.schema != installationSchema || !terminated(config.ssid) || !config.ssid[0]
        || !strcmp(config.ssid, "your-network-name") || !strcmp(config.ssid, "your-ssid")
        || !terminated(config.password) || !strcmp(config.password, "your-network-password")
        || !strcmp(config.password, "your-password") || !terminated(config.host)
        || !terminated(config.timezone) || !config.timezone[0] || !slug(config.profile)
        || !terminated(config.pairing) || !terminated(config.certificate) || !terminated(config.deviceKey)
        || !terminated(config.zip) || !terminated(config.place) || !terminated(config.callSign)
        || config.units > 1 || config.clock24h > 1 || config.callState >= callStateCount)
        return false;
    const bool quietOff = config.quietStart == -1 && config.quietEnd == -1;
    auto minute = [](int16_t m) { return m >= 0 && m < 24 * 60; };
    if (!quietOff && !(minute(config.quietStart) && minute(config.quietEnd))) return false;
    const size_t call = strlen(config.callSign);
    if (call && (call < 3 || strspn(config.callSign, "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789") != call))
        return false;
    const size_t zip = strlen(config.zip);
    const bool zipOk = (zip == 5 || (zip == 10 && config.zip[5] == '-'))
        && strspn(config.zip, "0123456789") == 5
        && (zip == 5 || strspn(config.zip + 6, "0123456789") == 4);
    if (zip && !zipOk) return false;
    const bool hasLat = std::isfinite(config.latitude), hasLon = std::isfinite(config.longitude);
    if (hasLat != hasLon) return false;
    if (hasLat && (std::fabs(config.latitude) > 90.f || std::fabs(config.longitude) > 180.f)) return false;
    if (standalone(config))
        return !config.pairing[0] && !config.certificate[0] && !config.deviceKey[0] && (zip || hasLat);
    const bool tls = !strncmp(config.host, "https://", 8);
    if (!tls && strncmp(config.host, "http://", 7)) return false;
    const char* authority = config.host + (tls ? 8 : 7);
    const size_t length = strcspn(authority, "/");
    if (!length || strpbrk(authority, "?#@ \t\r\n")
        || strspn(authority + length, "/") != strlen(authority + length)) return false;
    if (!tls) return !config.pairing[0] && !config.certificate[0] && !config.deviceKey[0];
    return slug(config.pairing) && config.certificate[0] && strlen(config.deviceKey) == 64
        && strspn(config.deviceKey, "0123456789abcdef") == 64;
}

inline bool installationTransportAllowed(const Installation& config, bool bench) {
    // Host transport only; a standalone record has no host to talk to.
    return validInstallation(config) && !standalone(config)
        && (bench || !strncmp(config.host, "https://", 8));
}

// The loader read `length` stored bytes over a default record, so a whole
// schema-1 blob already sits in place with the schema-2 fields at their
// defaults; only the schema number changes.
// ---- Storage --------------------------------------------------------------
//
// NVS here is a 20 KB partition shared with Wi-Fi, PHY calibration and the
// cache bookkeeping. Rewriting a large blob in place when it is nearly full
// can lose the old value along with the failed new one (seen on the bench in
// 1.1.0 development), so the record is never rewritten in place:
//   * two slots, "inst-a" and "inst-b", each a header (magic, generation,
//     length, CRC-32) and a compact payload: the struct without the unused
//     tail of the 4 KB certificate field (a standalone record is ~0.7 KB);
//   * a save goes to the slot that does not hold the newest record, is read
//     back, and only then is the older slot (and any legacy record) removed;
//   * the pre-1.1 "installation" key is read when no slot is usable and is
//     removed only after its migrated copy is safely in a slot.

inline constexpr char installationSlotA[] = "inst-a";
inline constexpr char installationSlotB[] = "inst-b";
inline constexpr uint32_t installationBlobMagic = 0x32524956;  // "VIR2"
inline constexpr size_t installationBlobHeader = 16;
inline constexpr size_t installationCertOffset = offsetof(Installation, certificate);
inline constexpr size_t installationTailOffset = offsetof(Installation, deviceKey);
inline constexpr size_t installationTailBytes = sizeof(Installation) - installationTailOffset;
static_assert(installationTailOffset == installationCertOffset + sizeof(Installation{}.certificate));

inline uint32_t installationCrc32(const uint8_t* data, size_t length) {
    uint32_t crc = 0xffffffffu;
    for (size_t i = 0; i < length; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

inline std::vector<uint8_t> packInstallation(const Installation& config) {
    const auto* bytes = reinterpret_cast<const uint8_t*>(&config);
    const size_t cert = strnlen(config.certificate, sizeof config.certificate - 1) + 1;
    std::vector<uint8_t> payload(bytes, bytes + installationCertOffset);
    payload.insert(payload.end(), bytes + installationCertOffset, bytes + installationCertOffset + cert);
    payload.back() = 0;
    payload.insert(payload.end(), bytes + installationTailOffset, bytes + sizeof config);
    return payload;
}

inline bool unpackInstallation(const uint8_t* payload, size_t length, Installation& out) {
    if (length < installationCertOffset + 1 + installationTailBytes) return false;
    const size_t cert = length - installationCertOffset - installationTailBytes;
    const uint8_t* text = payload + installationCertOffset;
    if (cert > sizeof out.certificate || text[cert - 1] || memchr(text, 0, cert - 1)) return false;
    out = Installation{};
    auto* bytes = reinterpret_cast<uint8_t*>(&out);
    memcpy(bytes, payload, installationCertOffset);
    memcpy(bytes + installationCertOffset, text, cert);
    memcpy(bytes + installationTailOffset, text + cert, installationTailBytes);
    return true;
}

inline void putLe32(std::vector<uint8_t>& out, size_t at, uint32_t value) {
    for (int i = 0; i < 4; ++i) out[at + i] = uint8_t(value >> (8 * i));
}
inline uint32_t getLe32(const uint8_t* in) {
    return uint32_t(in[0]) | uint32_t(in[1]) << 8 | uint32_t(in[2]) << 16 | uint32_t(in[3]) << 24;
}

inline std::vector<uint8_t> encodeInstallationBlob(const std::vector<uint8_t>& payload, uint32_t generation) {
    std::vector<uint8_t> blob(installationBlobHeader);
    putLe32(blob, 0, installationBlobMagic);
    putLe32(blob, 4, generation);
    putLe32(blob, 8, uint32_t(payload.size()));
    putLe32(blob, 12, installationCrc32(payload.data(), payload.size()));
    blob.insert(blob.end(), payload.begin(), payload.end());
    return blob;
}
#ifdef VENTANAL_HOST_TEST
inline std::vector<uint8_t> encodeInstallationBlobForTest(const std::vector<uint8_t>& payload, uint32_t g) {
    return encodeInstallationBlob(payload, g);
}
#endif

enum class SlotState { Absent, Corrupt, Future, Ok };

struct InstallationSlot {
    SlotState state = SlotState::Absent;
    uint32_t generation = 0;
    std::unique_ptr<Installation> record;  // heap: the struct is 4.8 KB
};

inline InstallationSlot readInstallationSlot(Preferences& prefs, const char* key) {
    InstallationSlot slot;
    const size_t length = prefs.getBytesLength(key);
    if (!length) return slot;
    slot.state = SlotState::Corrupt;
    if (length < installationBlobHeader || length > installationBlobHeader + sizeof(Installation)) return slot;
    std::vector<uint8_t> blob(length);
    if (prefs.getBytes(key, blob.data(), length) != length) return slot;
    const uint8_t* payload = blob.data() + installationBlobHeader;
    const size_t payloadLength = length - installationBlobHeader;
    if (getLe32(blob.data()) != installationBlobMagic || getLe32(blob.data() + 8) != payloadLength
        || getLe32(blob.data() + 12) != installationCrc32(payload, payloadLength))
        return slot;
    auto record = std::make_unique<Installation>();
    if (!unpackInstallation(payload, payloadLength, *record)) return slot;
    if (record->schema > installationSchema) {
        slot.state = SlotState::Future;
        return slot;
    }
    if (record->schema != installationSchema || !validInstallation(*record)) return slot;
    slot.state = SlotState::Ok;
    slot.generation = getLe32(blob.data() + 4);
    slot.record = std::move(record);
    return slot;
}

inline const InstallationSlot* newestSlot(const InstallationSlot& a, const InstallationSlot& b) {
    if (a.state != SlotState::Ok) return b.state == SlotState::Ok ? &b : nullptr;
    if (b.state != SlotState::Ok) return &a;
    return b.generation > a.generation ? &b : &a;
}

// Saves a valid record without ever touching the newest stored copy first.
inline bool saveInstallation(const Installation& config) {
    if (!validInstallation(config)) return false;
    Preferences prefs;
    if (!prefs.begin(installationNamespace, false)) return false;
    const InstallationSlot a = readInstallationSlot(prefs, installationSlotA);
    const InstallationSlot b = readInstallationSlot(prefs, installationSlotB);
    const InstallationSlot* newest = newestSlot(a, b);
    const bool targetB = newest == &a || (!newest && a.state == SlotState::Future);
    const char* target = targetB ? installationSlotB : installationSlotA;
    const char* other = targetB ? installationSlotA : installationSlotB;
    const std::vector<uint8_t> blob = encodeInstallationBlob(packInstallation(config),
                                                             newest ? newest->generation + 1 : 1);
    bool saved = prefs.putBytes(target, blob.data(), blob.size()) == blob.size();
    if (saved) {
        std::vector<uint8_t> check(blob.size());
        saved = prefs.getBytesLength(target) == blob.size()
            && prefs.getBytes(target, check.data(), check.size()) == check.size() && check == blob;
    }
    if (saved) {
        if (prefs.isKey(other)) prefs.remove(other);
        if (prefs.isKey(installationKey)) prefs.remove(installationKey);
    }
    prefs.end();
    return saved;
}

// The pre-1.1 record under "installation": schema 1 (4700 bytes), or a whole
// schema-2 struct from 1.1.0 development builds. Never rewritten here.
inline SlotState readLegacyInstallation(Preferences& prefs, Installation& out) {
    const size_t length = prefs.getBytesLength(installationKey);
    if (!length) return SlotState::Absent;
    if (length != installationSchema1Bytes && length != sizeof(Installation)) return SlotState::Corrupt;
    auto record = std::make_unique<Installation>();
    if (prefs.getBytes(installationKey, record.get(), length) != length) return SlotState::Corrupt;
    if (record->schema > installationSchema) return SlotState::Future;
    const bool schema1 = length == installationSchema1Bytes && record->schema == 1;
    const bool schema2 = length == sizeof(Installation) && record->schema == installationSchema;
    if (!schema1 && !schema2) return SlotState::Corrupt;
    record->schema = installationSchema;  // schema-2 fields kept their defaults past 4700 bytes
    if (!validInstallation(*record)) return SlotState::Corrupt;
    out = *record;
    return SlotState::Ok;
}

// Loads the newest usable record. A valid seed (private builds) is written
// only when nothing usable is stored or its forced revision changed; a record
// from newer firmware is left alone. persistMigration=false (an update not yet
// confirmed) never writes.
inline bool loadInstallation(Installation& out, const Installation* seed = nullptr,
                             uint32_t forceRevision = 0, bool persistMigration = true) {
    out = Installation{};
    const bool canSeed = seed && validInstallation(*seed);
    bool usable = false, legacy = false;
    {
        Preferences prefs;
        if (!prefs.begin(installationNamespace, !canSeed)) return false;  // read-only start creates nothing
        const InstallationSlot a = readInstallationSlot(prefs, installationSlotA);
        const InstallationSlot b = readInstallationSlot(prefs, installationSlotB);
        if (a.state == SlotState::Future || b.state == SlotState::Future) return false;
        if (const InstallationSlot* newest = newestSlot(a, b)) {
            out = *newest->record;
            usable = true;
        } else {
            const SlotState old = readLegacyInstallation(prefs, out);
            if (old == SlotState::Future) return false;
            usable = legacy = old == SlotState::Ok;
        }
    }
    if (canSeed && (!usable || (forceRevision && out.forcedRevision != forceRevision))) {
        auto replacement = std::make_unique<Installation>(*seed);
        replacement->forcedRevision = forceRevision;
        if (saveInstallation(*replacement)) {
            out = *replacement;
            return true;
        }
    } else if (legacy && persistMigration) {
        saveInstallation(out);  // on failure the legacy record stays for the next boot
    }
    if (!usable) out = Installation{};
    return usable;
}

#ifdef VENTANAL_HOST_TEST
// The network harness always uses the public sample's timezone and Wi-Fi.
inline Installation installation{installationSchema, 0, "your-network-name",
    "your-network-password", "", "EST5EDT,M3.2.0,M11.1.0"};
#else
inline Installation installation{};
bool initializeInstallation(bool pendingUpdate);
#endif
}  // namespace lcd
