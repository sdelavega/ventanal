// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
#include "sd_card.h"
#include "board.h"
#include "ch422g.h"
#include "../common/resume/fs_store.cpp"
#include "../common/resume/storage.cpp"
#include <SD_MMC.h>

extern Ch422g expander;

bool mountSdCard() {
    // DAT3 is on CH422G EXIO4. Hold it high during CMD0 to enter native SD
    // mode, then use the three GPIO lines in 1-bit mode.
    if (!expander.setSdSelected(false) ||
        !SD_MMC.setPins(lcd::SD_CLK_GPIO, lcd::SD_CMD_GPIO, lcd::SD_D0_GPIO) ||
        !SD_MMC.begin("/sdcard", true, false)) return false;
    if (!probePartialStorage(SD_MMC)) {
        SD_MMC.end();
        return false;
    }
    setPartialSdFilesystem(SD_MMC);
    return true;
}
