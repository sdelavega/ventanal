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
            int battery, int quietStart, int quietEnd, void (*feed)());
