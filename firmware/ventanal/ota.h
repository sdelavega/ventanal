// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
#pragma once

#include "logic.h"
#include <cstdio>
#include <cstring>

class Preferences;

// Layout values are checked against partitions.csv and upload.py by test_upload.py.
inline constexpr size_t otaCacheOffset = 0x820000;
inline constexpr size_t otaCacheSize = 0x17D0000;
inline constexpr size_t otaSlotSize = 0x400000;
inline constexpr size_t otaVersionCapacity = sizeof("65535.65535.65535");
inline constexpr size_t otaReportCapacity = sizeof("rolled_back:65535.65535.65535");

inline constexpr size_t otaApprovalCapacity = 17;
inline constexpr uint8_t otaAttemptLimit = 3;

inline bool validOtaApproval(const char* id) {
    if (strlen(id) != otaApprovalCapacity - 1) return false;
    return strspn(id, "0123456789abcdef") == otaApprovalCapacity - 1;
}

struct FirmwareOffer {
    char release[2049];
    char signature[89];
    char path[64];
    char approval[otaApprovalCapacity];
};
inline FirmwareOffer manifestFirmware{};

enum class OtaResult : uint8_t { None, Installing, Ok, RolledBack, Failed };
enum class OtaFailure : uint8_t {
    None, Release, Path, Slot, Download, Write, Digest, Schedule, Interrupted, Attempts
};
struct OtaRecord {
    OtaResult result{};
    OtaFailure failure{};
    uint8_t slot{}; // For failed:attempts, this byte holds the last failure instead.
    char version[otaVersionCapacity]{};
};

inline bool validOtaRecord(const OtaRecord& record) {
    if (record.result > OtaResult::Failed || record.failure > OtaFailure::Attempts
        || !memchr(record.version, '\0', sizeof record.version)) return false;
    if (record.result == OtaResult::Installing || record.result == OtaResult::Ok
        || record.result == OtaResult::RolledBack)
        return record.version[0] && strspn(record.version, "0123456789.") == strlen(record.version);
    return true;
}

inline void setOtaFailure(OtaRecord& record, OtaFailure reason, bool exhausted) {
    record.result = OtaResult::Failed;
    record.failure = exhausted ? OtaFailure::Attempts : reason;
    record.slot = exhausted ? static_cast<uint8_t>(reason) : 0;
}

enum class OtaSlotState { Other, Pending, Valid, Rejected };

inline OtaResult otaBootResult(const OtaRecord& record, bool runningTarget,
                               OtaSlotState targetState, bool versionMatches) {
    if (record.result != OtaResult::Installing) return record.result;
    if (runningTarget && targetState == OtaSlotState::Pending) return OtaResult::Installing;
    if (runningTarget && targetState == OtaSlotState::Valid && versionMatches) return OtaResult::Ok;
    if (!runningTarget && targetState == OtaSlotState::Rejected) return OtaResult::RolledBack;
    return OtaResult::Failed;
}

inline bool otaCanInstall(int battery, int minute, int quietStart, int quietEnd) {
    return battery >= 50 && battery <= 100 && minute >= 0 && minute < 1440
        && quietStart >= 0 && quietStart < 1440 && quietEnd >= 0 && quietEnd < 1440
        && !inQuiet(minute, quietStart, quietEnd);
}

inline bool otaCanConfirm(bool pending, bool versionMatches, bool manifestOk, bool screenDrawn) {
    return pending && versionMatches && manifestOk && screenDrawn;
}

inline bool otaCanAcknowledge(OtaResult result, bool rolledBackTargetOffered) {
    return result == OtaResult::Ok || result == OtaResult::Failed
        || (result == OtaResult::RolledBack && !rolledBackTargetOffered);
}

inline void formatOtaReport(char (&out)[otaReportCapacity], const OtaRecord& record) {
    out[0] = '\0';
    switch (record.result) {
        case OtaResult::Ok:
            snprintf(out, sizeof out, "ok:%s", record.version);
            break;
        case OtaResult::RolledBack:
            snprintf(out, sizeof out, "rolled_back:%s", record.version);
            break;
        case OtaResult::Failed: {
            constexpr const char* reasons[] = {
                "unknown", "release", "path", "slot", "download", "write", "digest",
                "schedule", "interrupted", "attempts"
            };
            const auto reason = static_cast<unsigned>(record.failure);
            const auto last = record.slot;
            if (record.failure == OtaFailure::Attempts && last && last < sizeof reasons / sizeof *reasons)
                snprintf(out, sizeof out, "failed:attempts:%s",
                         reasons[last]);
            else
                snprintf(out, sizeof out, "failed:%s",
                         reasons[reason < sizeof reasons / sizeof *reasons ? reason : 0]);
            break;
        }
        default:
            break;
    }
}

struct OtaAttempts {
    char version[otaVersionCapacity]{};
    char approval[otaApprovalCapacity]{};
    uint8_t count{};
};

inline bool reserveOtaAttempt(OtaAttempts& attempts, const char* version, const char* approval) {
    if (strcmp(attempts.approval, approval) || (!approval[0] && strcmp(attempts.version, version))) {
        attempts = {};
        snprintf(attempts.version, sizeof attempts.version, "%s", version);
        snprintf(attempts.approval, sizeof attempts.approval, "%s", approval);
    }
    if (attempts.count >= otaAttemptLimit) return false;
    ++attempts.count;
    return true;
}

inline bool otaGithubCheckDue(bool synced, int64_t now, int64_t last) {
    constexpr int64_t interval = 7 * 24 * 60 * 60;
    return synced && now >= 1704067200 && (last <= 0 || (now >= last && now - last >= interval));
}

struct OtaBoot {
    OtaRecord record{};
    OtaAttempts attempts{};
    bool pending = false;
    bool matching = false;
    bool rolledBack = false;
};

OtaBoot readOtaBoot(Preferences& prefs, bool persisted);
void acknowledgeOtaReport(Preferences& prefs, OtaBoot& boot);
bool confirmOtaBoot(Preferences& prefs, OtaBoot& boot, bool manifestOk, bool screenDrawn);
void rollbackOtaBoot();
bool otaCacheLayout();
// Returns true when a failed attempt should defer frame downloads.
bool tryOta(Preferences& prefs, const OtaBoot& boot, bool manifestOk, bool timeSynced,
            int battery, int quietStart, int quietEnd);
