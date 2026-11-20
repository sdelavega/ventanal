// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
#include "sd_card.h"
#include <Inkplate.h>
#include <SD.h>
#include "../common/resume/fs_store.cpp"
#include "../common/resume/storage.cpp"

extern Inkplate display;

namespace {
// Inkplate13SPECTRA driver sdCardInit() and sdCardSleep() use these pins.
constexpr int kCs = 10, kMosi = 11, kSck = 12, kMiso = 13;
bool mounted = false;
}

bool mountSdCard() {
    // The library powers the slot and initializes its dedicated SPI bus.
    // Release SdFat before handing the bus to Arduino's fs::FS adapter.
    const bool ready = display.sdCardInit();
    display.getSdFat().end();
    display.setSdCardOk(0);
    SPIClass& bus = *display.getSPIptr();
    if (ready && bus.begin(kSck, kMiso, kMosi, kCs) &&
        SD.begin(kCs, bus, 20000000) && probePartialStorage(SD)) {
        mounted = true;
        setPartialSdFilesystem(SD);
        return true;
    }
    SD.end();
    bus.end();
    display.sdCardSleep();
    return false;
}

void unmountSdCard() {
    if (!mounted) return;
    SD.end();
    display.getSPIptr()->end();
    display.sdCardSleep();
    mounted = false;
}
