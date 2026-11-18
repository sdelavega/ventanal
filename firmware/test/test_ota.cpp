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
    puts("OTA policy: battery and quiet hours, install recovery, healthy boot and result fields passed");
}
