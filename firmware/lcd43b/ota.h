// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
#pragma once

#include "../common/ota_state.h"
#include "../common/resume/resume.h"

class Preferences;
namespace lcd {
class Watchdog;

inline constexpr size_t otaSlotSize = 0x400000;
inline constexpr size_t otaCacheOffset = 0x810000;
inline constexpr size_t otaCacheSize = 0x7F0000;

enum class UpdateSource : uint8_t { None, Host, GitHub };

struct UpdateOffer {
    FirmwareOffer signedOffer{};
    char version[otaVersionCapacity]{};
};

struct UpdateJournal {
    OtaRecord record{};
    UpdateSource source{};
    char approval[otaApprovalCapacity]{};
};

struct UpdateBoot {
    UpdateJournal journal{};
    bool pending = false;
    bool matching = false;
};

struct GithubCheck {
    int64_t checked = 0;
    FirmwareOffer offer{};
};

enum class CheckResult { Never, NotDue, UpToDate, Available, Failed };

inline bool githubCheckDue(bool standalone, int64_t lastCheck, int64_t now) {
    return standalone && otaGithubCheckDue(true, now, lastCheck);
}

bool validateOffer(const FirmwareOffer& input, UpdateSource source, UpdateOffer& out);
bool sameOffer(const UpdateOffer& a, const UpdateOffer& b);
bool rolledBackOffer(const UpdateBoot& boot, const UpdateOffer& offer);
bool mountUpdateCache(Preferences& prefs);
UpdateBoot readUpdateBoot(Preferences& prefs);
bool armUpdateDeadline();
void acknowledgeUpdate(Preferences& prefs, UpdateBoot& boot, const char* sentReport,
                       const char* sentApproval);
bool confirmUpdate(Preferences& prefs, UpdateBoot& boot);
[[noreturn]] void rollbackUpdate();

FetchOutcome stageUpdatePass(Preferences& prefs, const UpdateBoot& boot, const UpdateOffer& offer,
                             UpdateSource source, Watchdog& watchdog, int protectedSpare,
                             OtaFailure& failure);
void recordUpdateFailure(Preferences& prefs, UpdateBoot& boot, const UpdateOffer& offer,
                         UpdateSource source, OtaFailure failure);
OtaFailure installUpdate(Preferences& prefs, UpdateBoot& boot, const UpdateOffer& offer,
                         UpdateSource source, Watchdog& watchdog);
CheckResult checkGithub(int64_t last, GithubCheck& result, Watchdog& watchdog);
int64_t loadGithubCheck(Preferences& prefs, UpdateOffer& offer);
bool saveGithubCheck(Preferences& prefs, const GithubCheck& result);
int64_t loadGithubAutoFailure(Preferences& prefs);
bool saveGithubAutoFailure(Preferences& prefs, int64_t attemptedAt);
}  // namespace lcd
