// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
#pragma once

#include "logic.h"
#include <cstdio>
#include <cstring>

class Preferences;

inline constexpr size_t otaSlotSize = 4 * 1024 * 1024;
inline constexpr size_t otaVersionCapacity = sizeof("65535.65535.65535");
inline constexpr size_t otaReportCapacity = sizeof("rolled_back:65535.65535.65535");

struct FirmwareOffer {
    char release[2049];
    char signature[89];
    char path[64];
};
inline FirmwareOffer manifestFirmware{};

enum class OtaResult : uint8_t { None, Installing, Ok, RolledBack, Failed };
enum class OtaFailure : uint8_t {
    None, Release, Path, Slot, Download, Write, Digest, Schedule, Interrupted
};
struct OtaRecord {
    OtaResult result{};
    OtaFailure failure{};
    uint8_t slot{};
    char version[otaVersionCapacity]{};
};

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
                "schedule", "interrupted"
            };
            const auto reason = static_cast<unsigned>(record.failure);
            snprintf(out, sizeof out, "failed:%s",
                     reasons[reason < sizeof reasons / sizeof *reasons ? reason : 0]);
            break;
        }
        default:
            break;
    }
}

struct OtaBoot {
    OtaRecord record{};
    bool pending = false;
    bool matching = false;
    bool rolledBack = false;
};

OtaBoot readOtaBoot(Preferences& prefs, bool persisted);
void acknowledgeOtaReport(Preferences& prefs, OtaBoot& boot);
bool confirmOtaBoot(Preferences& prefs, OtaBoot& boot, bool manifestOk, bool screenDrawn);
void rollbackOtaBoot();
bool otaCacheLayout();
void tryOta(Preferences& prefs, const OtaBoot& boot, bool manifestOk,
            int battery, int quietStart, int quietEnd);
