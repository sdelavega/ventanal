// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
#pragma once

#include "logic.h"
#include "../common/ota_state.h"

class Preferences;

// Layout values are checked against partitions.csv and upload.py by test_upload.py.
inline constexpr size_t otaCacheOffset = 0x820000;
inline constexpr size_t otaCacheSize = 0x17D0000;
inline constexpr size_t otaSlotSize = 0x400000;
inline FirmwareOffer manifestFirmware{};

inline bool otaCanInstall(int battery, int minute, int quietStart, int quietEnd) {
    return battery >= 50 && battery <= 100 && minute >= 0 && minute < 1440
        && quietStart >= 0 && quietStart < 1440 && quietEnd >= 0 && quietEnd < 1440
        && !inQuiet(minute, quietStart, quietEnd);
}

inline constexpr uint8_t otaFreeDeferrals = 3;

struct OtaAttemptState {
    OtaAttempts attempts{};
    uint8_t deferrals{};
};

inline bool scopeOtaAttemptState(OtaAttemptState& state, const char* version, const char* approval) {
    if (!strcmp(state.attempts.version, version) && !strcmp(state.attempts.approval, approval))
        return false;
    scopeOtaAttempts(state.attempts, version, approval);
    snprintf(state.attempts.version, sizeof state.attempts.version, "%s", version);
    state.deferrals = 0;
    return true;
}

// Once the free deferrals are used, charge each further deferral to the budget.
inline OtaFailure updateOtaAttemptState(OtaAttemptState& state, const char* version,
                                        const char* approval, bool deferred) {
    scopeOtaAttemptState(state, version, approval);
    if (state.attempts.count >= otaAttemptLimit) return OtaFailure::Attempts;
    if (deferred && state.deferrals < otaFreeDeferrals) {
        ++state.deferrals;
        return OtaFailure::None;
    }
    reserveOtaAttempt(state.attempts, version, approval);
    if (deferred) return OtaFailure::Deferred;
    state.deferrals = 0;
    return OtaFailure::None;
}

struct OtaBoot {
    OtaRecord record{};
    OtaAttemptState attemptState{};
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
            int battery, int quietStart, int quietEnd, void (*feed)());
