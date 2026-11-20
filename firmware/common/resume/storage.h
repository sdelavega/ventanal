// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
#pragma once

#include "fs_store.h"

// The board registers its filesystem after mounting and probing it.
void setPartialSdFilesystem(fs::FS& fs, uint64_t (*freeBytes)() = nullptr);
uint64_t partialFreeBytes();
void partialStoreFull();
bool partialsOnFlash();
FsStore& partialStore();
FsStore& flashPartialStore();
void selectPartialStore(bool sdMounted);
void setPartialStoreWriteHook(void (*hook)(bool));
void forEachPartialStore(const std::function<void(FsStore&)>& visit);
void sweepPartials(uint32_t epochNow, bool (*validatorLive)(const PartialMeta&));
