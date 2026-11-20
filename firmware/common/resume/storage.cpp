// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
#include "storage.h"
#include "fs_store.h"
#include "resume.h"
#include <LittleFS.h>
#include <atomic>
#include <optional>

namespace {
FsStore flash(LittleFS);
std::optional<FsStore> sd;
std::atomic<bool> useSd{false};

void fallBackToFlash() {
    if (useSd.exchange(false)) Serial.println("storage partials=flash");
}
}

void setPartialSdFilesystem(fs::FS& fs) {
    sd.emplace(fs, "/partials", fallBackToFlash);
}

PartialStore& partialStore() {
    if (useSd.load()) return *sd;
    return flash;
}

void selectPartialStore(bool sdMounted) {
    const bool selected = sdMounted && sd.has_value();
    useSd.store(selected);
    Serial.println(selected ? "storage partials=sd" : "storage partials=flash");
}

void sweepPartials(uint32_t epochNow, bool (*validatorLive)(const PartialMeta&)) {
    if (epochNow < resume_detail::kValidEpoch) return;
    if (useSd.load()) sd->sweep(epochNow, validatorLive);
    flash.sweep(epochNow, validatorLive);
}
