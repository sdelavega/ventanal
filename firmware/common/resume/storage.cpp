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
uint64_t (*sdFreeBytes)() = nullptr;
void (*writeHook)(bool) = nullptr;

void fallBackToFlash() {
    if (useSd.exchange(false)) Serial.println("storage partials=flash");
}
}

void setPartialSdFilesystem(fs::FS& fs, uint64_t (*freeBytes)()) {
    sdFreeBytes = freeBytes;
    sd.emplace(fs, "/partials", fallBackToFlash);
    sd->setWriteHook(writeHook);
}

void partialStoreFull() { fallBackToFlash(); }

bool partialsOnFlash() { return !useSd.load(); }

uint64_t partialFreeBytes() {
    if (!partialsOnFlash()) {
        return sdFreeBytes ? sdFreeBytes() : 0;
    }
    const auto total = LittleFS.totalBytes();
    const auto used = LittleFS.usedBytes();
    return total > used ? total - used : 0;
}

FsStore& partialStore() {
    if (useSd.load()) return *sd;
    return flash;
}

FsStore& flashPartialStore() { return flash; }

void selectPartialStore(bool sdMounted) {
    const bool selected = sdMounted && sd.has_value();
    useSd.store(selected);
    Serial.println(selected ? "storage partials=sd" : "storage partials=flash");
}

void setPartialStoreWriteHook(void (*hook)(bool)) {
    writeHook = hook;
    flash.setWriteHook(hook);
    if (sd) sd->setWriteHook(hook);
}

void forEachPartialStore(const std::function<void(FsStore&)>& visit) {
    if (useSd.load()) {
        visit(*sd);
        visit(flash);
    } else {
        visit(flash);
        if (sd) visit(*sd);
    }
}

void sweepPartials(uint32_t epochNow, bool (*validatorLive)(const PartialMeta&)) {
    if (epochNow < resume_detail::kValidEpoch) return;
    if (useSd.load()) sd->sweep(epochNow, validatorLive);
    flash.sweep(epochNow, validatorLive);
}
