// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
#include "../ventanal/ota.h"
#include <cassert>

int main() {
    // Overnight quiet hours include the start and exclude the end.
    assert(!otaCanInstall(49, 360, 1380, 360));
    assert(!otaCanInstall(100, -1, 1380, 360));
    assert(!otaCanInstall(101, 360, 1380, 360));
    assert(!otaCanInstall(100, 360, -1, 360));
    assert(!otaCanInstall(50, 0, 1380, 360));
    assert(!otaCanInstall(50, 1380, 1380, 360));
    assert(otaCanInstall(50, 360, 1380, 360));
    assert(otaCanInstall(50, 1379, 1380, 360));
    assert(!otaCanInstall(50, 700, 600, 720));
    assert(otaCanInstall(50, 700, 600, 600));

    OtaRecord record{};
    assert(otaBootResult(record, false, OtaSlotState::Other, false) == OtaResult::None);
    record = {OtaResult::Installing, OtaFailure::None, 0x11, "2.0.0"};
    // Reset after the journal, before the boot partition was selected.
    assert(otaBootResult(record, false, OtaSlotState::Other, false) == OtaResult::Failed);
    // First boot in the new slot waits for the application check.
    assert(otaBootResult(record, true, OtaSlotState::Pending, true) == OtaResult::Installing);
    assert(otaCanConfirm(true, true, true, true));
    assert(!otaCanConfirm(false, true, true, true));
    assert(!otaCanConfirm(true, false, true, true));
    assert(!otaCanConfirm(true, true, false, true));
    assert(!otaCanConfirm(true, true, true, false));
    // Reset between validating the image and saving the result.
    assert(otaBootResult(record, true, OtaSlotState::Valid, true) == OtaResult::Ok);
    assert(otaBootResult(record, true, OtaSlotState::Valid, false) == OtaResult::Failed);
    assert(otaBootResult(record, false, OtaSlotState::Rejected, false) == OtaResult::RolledBack);

    assert(!otaCanAcknowledge(OtaResult::Installing, false));
    assert(!otaCanAcknowledge(OtaResult::RolledBack, true));
    assert(otaCanAcknowledge(OtaResult::RolledBack, false));
    assert(otaCanAcknowledge(OtaResult::Ok, false));
    assert(otaCanAcknowledge(OtaResult::Failed, true));

    char report[otaReportCapacity];
    formatOtaReport(report, record);
    assert(!report[0]);
    record.result = OtaResult::Ok;
    formatOtaReport(report, record);
    assert(!strcmp(report, "ok:2.0.0"));
    record.result = OtaResult::RolledBack;
    strcpy(record.version, "65535.65535.65535");
    formatOtaReport(report, record);
    assert(!strcmp(report, "rolled_back:65535.65535.65535"));
    record.result = OtaResult::Failed;
    record.failure = OtaFailure::Interrupted;
    formatOtaReport(report, record);
    assert(!strcmp(report, "failed:interrupted"));
    record.failure = OtaFailure::Attempts;
    formatOtaReport(report, record);
    assert(!strcmp(report, "failed:attempts"));

    OtaAttempts attempts{};
    const char* approval = "0123456789abcdef";
    assert(validOtaApproval(approval) && !validOtaApproval("0123456789abcdeF"));
    for (int i = 0; i < 3; ++i) assert(reserveOtaAttempt(attempts, "2.0.0", approval));
    assert(!reserveOtaAttempt(attempts, "2.0.0", approval));
    assert(reserveOtaAttempt(attempts, "2.0.0", "1123456789abcdef") && attempts.count == 1);
    assert(reserveOtaAttempt(attempts, "2.1.0", "1123456789abcdef") && attempts.count == 1);

    constexpr int64_t now = 4500000000LL;
    constexpr int64_t week = 604800;
    assert(!otaGithubCheckDue(false, now, 0));
    assert(!otaGithubCheckDue(true, 0, 0));
    assert(otaGithubCheckDue(true, now, 0));
    assert(otaGithubCheckDue(true, now, now - week));
    assert(!otaGithubCheckDue(true, now, now - week + 1));
    assert(!otaGithubCheckDue(true, now, now + 1));

    puts("OTA policy: battery and quiet hours, install recovery, healthy boot and result fields passed");
}
