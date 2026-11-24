// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
//
// Lit-write policy, refresh worker, applying results and deferred saves.
//
// Part of lcd43b.ino's single translation unit: included only by lcd43b.ino,
// inside its anonymous namespace, in the order listed there. Earlier parts
// provide the declarations later parts use. Do not include it anywhere else.
#pragma once
#if defined(VENTANAL_LCD_LIT_NVS) && defined(CONFIG_SPIRAM_XIP_FROM_PSRAM)
constexpr bool LitNvsWrites = true;
#else
constexpr bool LitNvsWrites = false;
#endif
#if defined(VENTANAL_LCD_LIT_CACHE) && defined(CONFIG_SPIRAM_XIP_FROM_PSRAM)
constexpr bool LitCacheWrites = true;
#else
constexpr bool LitCacheWrites = false;
#endif

constexpr uint32_t LitSaveIdleMs = 5000;

bool litFrameSaveAllowed(uint32_t now) {
    return LitCacheWrites && !settingsPage.active && installQueued == lcd::UpdateSource::None
        && powerSession.idle(now) >= LitSaveIdleMs;
}

// Keep the conservative pause at write sites until their lit XIP path
// is measured. Give scanout time to settle before restoring the backlight.
class ScanoutSavePause {
public:
    explicit ScanoutSavePause(bool saving = true) : restore_(saving && backlightState) {
        if (!restore_) return;
        displayOwner.acquire(lcd::DarkReason::Save, millis());
        backlightRefresh();
    }
    ~ScanoutSavePause() {
        if (!restore_) return;
        if (!(panel.waitForVsync() && panel.waitForVsync() && panel.waitForPresent()))
            lcd::log("panel save pause scanout=timeout\n");
        displayOwner.release(lcd::DarkReason::Save);
        backlightRefresh();
    }
private:
    bool restore_;
};

// --- Refresh worker ------------------------------------------------------

// Task-watchdog subscription for the worker: armed once at boot, the
// worker feeds it on every progress step (sync.cpp), so a wedged network
// call beyond the window resets the board instead of freezing refreshes.
class TaskWatchdog : public lcd::Watchdog {
public:
    void arm() {
        // The Arduino core starts its own TWDT, so reconfigure it here
        // (quietly); a system without one initialises it fresh instead.
        const esp_task_wdt_config_t config{lcd::WATCHDOG_TIMEOUT_MS, 0, true};
        const esp_err_t result = esp_task_wdt_reconfigure(&config);
        if (result == ESP_ERR_INVALID_STATE)
            ESP_ERROR_CHECK(esp_task_wdt_init(&config));
        else
            ESP_ERROR_CHECK(result);
    }
    void subscribe() {
        ESP_ERROR_CHECK(
            esp_task_wdt_add_user("ventanal-refresh", &handle_));
    }
    void feed() override { esp_task_wdt_reset_user(handle_); }
    void unsubscribe() { ESP_ERROR_CHECK(esp_task_wdt_delete_user(handle_)); }

private:
    esp_task_wdt_user_handle_t handle_ = nullptr;
};

TaskWatchdog refreshWatchdog;
SemaphoreHandle_t refreshStaged;   // worker -> loop: result ready to copy
SemaphoreHandle_t refreshCopied;   // loop -> worker: result applied
bool refreshRequested = false;
lcd::DeviceSettings refreshSettings; // copied before worker starts; immutable until it finishes
lcd::AcceptedUpdate refreshAcceptedUpdate;
lcd::UpdateSource refreshReadySource = lcd::UpdateSource::None;
FetchOutcome stagedUpdateOutcome = FetchOutcome::NoProgress;
OtaFailure stagedUpdateFailure = OtaFailure::None;
bool countStagedUpdateFailure = false;
lcd::SweepDay downloadSweepDay;
lcd::SweepDay frameSweepDay;
bool readyStageExpired = false;
char sweepDisplayedArt[17]{};
lcd::RefreshResult refreshStaging;   // worker writes, loop copies while parked
char refreshIds[lcd::SCREEN_SLOTS][17]{};
struct PendingFrame {
    int slot;
    const lcd::FrameInfo* info;
    const lcd::FrameBody* body;
    size_t requestedBytes = 0;
    int releaseSlot = -1;
    bool evictSpare = false;
    bool captureSweepContext = false;
};
PendingFrame pendingFrame;
SemaphoreHandle_t frameStaged;       // worker -> loop: decoded frame ready
SemaphoreHandle_t frameCopied;       // loop -> worker: scratch and body consumed

bool frameAccepted = false;

bool stageResidentFrame(int slot, const lcd::FrameInfo* info, const lcd::FrameBody& body) {
    pendingFrame = {slot, info, &body};
    xSemaphoreGive(frameStaged);
    xSemaphoreTake(frameCopied, portMAX_DELAY);
    return frameAccepted;
}

void requestWireMemory(size_t bytes) {
    // The worker parks just as it does for decoded-frame publication.
    // Only loop may release or recreate the scan-out buffers.
    pendingFrame = {-1, nullptr, nullptr, bytes};
    xSemaphoreGive(frameStaged);
    xSemaphoreTake(frameCopied, portMAX_DELAY);
}

bool requestReleaseFrame(int slot) {
    pendingFrame = {-1, nullptr, nullptr, 0, slot};
    xSemaphoreGive(frameStaged);
    xSemaphoreTake(frameCopied, portMAX_DELAY);
    return frameAccepted;
}

bool requestOtaSpareEviction() {
    pendingFrame = {-1, nullptr, nullptr, 0, -1, true};
    xSemaphoreGive(frameStaged);
    xSemaphoreTake(frameCopied, portMAX_DELAY);
    return frameAccepted;
}

void requestSweepContext() {
    pendingFrame = {-1, nullptr, nullptr, 0, -1, false, true};
    xSemaphoreGive(frameStaged);
    xSemaphoreTake(frameCopied, portMAX_DELAY);
}

uint8_t* clearOverrideSave() {
    deferredOverride = {};
    overrideSource = -1;
    uint8_t* retired = ownOverrideFrame ? overrideFrame : nullptr;
    overrideFrame = nullptr;
    ownOverrideFrame = false;
    return retired;
}

void applyResidentFrame() {
    const auto frame = pendingFrame;
    if (frame.captureSweepContext) {
        refreshAcceptedUpdate = acceptedUpdate;
        refreshReadySource = installQueued;
        const int slot = screenSlot(screenIndex);
        const char* artId = slot == 0 && owner.overrideArt.id[0] ? owner.overrideArt.id
            : slot >= 0 ? state.cachedIds[slot] : "";
        strlcpy(sweepDisplayedArt, artId, sizeof sweepDisplayedArt);
        xSemaphoreGive(frameCopied);
        return;
    }
    if (frame.evictSpare) {
        frameAccepted = false;
        lcd::ScanScope activity(lcd::ScanActivity::LittleFS);
        for (int slot = lcd::SCREEN_SLOTS - lcd::SPARE_SLOTS;
             slot < lcd::SCREEN_SLOTS && !frameAccepted; ++slot) {
            if (slot == overrideSource || slot == screenSlot(screenIndex)) continue;
            for (bool alternate : {false, true}) {
                if (LittleFS.remove(lcd::residentPath(slot, alternate).c_str())) {
                    frameAccepted = true;
                    break;
                }
            }
        }
        xSemaphoreGive(frameCopied);
        return;
    }
    if (frame.slot < 0 && frame.releaseSlot >= 0) {
        // Lend a spare's frame memory to the worker (standalone art). Never the
        // spare on screen or a pinned override; an empty id makes the slot read
        // as missing (screenFrame) until a new frame is published into it.
        const int slot = frame.releaseSlot;
        uint8_t* pixels = slot >= 3 && slot < lcd::SCREEN_SLOTS ? residentFrames[slot] : nullptr;
        frameAccepted = pixels && pixels != overrideFrame && screenSlot(screenIndex) != slot;
        if (frameAccepted) {
            residentFrames[slot] = nullptr;
            state.cachedIds[slot][0] = '\0';
            retirePixels(framePixels(pixels));
            free(pixels);
        }
        if (pixels || frameAccepted)
            lcd::log("cache release slot=%s ok=%d free_psram=%u\n", lcd::SLOT_NAMES[slot], frameAccepted,
                     unsigned(ESP.getFreePsram()));
        xSemaphoreGive(frameCopied);
        return;
    }
    if (frame.slot < 0) {
        if (!backlightState || litFrameSaveAllowed(millis())) reclaimCacheMemory(frame.requestedBytes);
        xSemaphoreGive(frameCopied);
        return;
    }
    auto& id = state.cachedIds[frame.slot];
    const char* nextId = frame.info ? frame.info->id : "";
    const bool changed = strcmp(id, nextId) != 0;
    // A retry of the same verified frame only needs to replace the save source.
    const bool saveRetry = !changed && frame.body->file && refreshCache.retryFileSave[frame.slot];
    uint8_t* retired = nullptr;
    if (frame.info && !saveRetry) {
        const bool pinnedSpare = residentFrames[frame.slot] && residentFrames[frame.slot] == overrideFrame;
        auto* replacement = pinnedSpare ? static_cast<uint8_t*>(ps_malloc(lcd::FRAME_BYTES)) : nullptr;
        if (pinnedSpare && !replacement) {
            frameAccepted = false;
            xSemaphoreGive(frameCopied);
            return;
        }
        std::swap(residentFrames[frame.slot], refreshCache.staging);
        if (pinnedSpare) {
            ownOverrideFrame = true;
            refreshCache.staging = replacement;
        } else retired = refreshCache.staging;
    }
    if (changed) {
        const uint32_t now = ventanal::trustedEpoch(time(nullptr));
        if (frame.info) contentStaleness.clearOverdue();
        if (frame.slot == 1) {
            // Standalone stamps the render. Host manifests carry no publish time,
            // so use the time the verified weather frame reached the live cache.
            weatherFrameEpoch = !frame.info ? 0
                : frame.info->renderedAt ? frame.info->renderedAt : now;
        }
    }
    const bool refreshFileSave = frame.body->file && strcmp(persistedFrames[frame.slot].id, nextId);
    if (mounted && (changed || refreshFileSave)) {
        auto& save = deferredFrames[frame.slot];
        if (save.replace(nextId, *frame.body, millis(), frame.slot == 1 ? weatherFrameEpoch : 0)) {
            lcd::log("cache queued slot=%s bytes=%u free_psram=%u\n", lcd::SLOT_NAMES[frame.slot],
                     unsigned(frame.body->size), unsigned(ESP.getFreePsram()));
        }
    }
    if (frame.body->file && deferredOverride.pending && deferredOverride.body.file &&
        deferredOverride.body.file->unavailable && !strcmp(deferredOverride.id, nextId))
        deferredOverride.replace(nextId, *frame.body, millis());
    strcpy(id, nextId);
    uint8_t* oldOverride = nullptr;
    if (frame.slot == 0 && strcmp(owner.mainId, id) != 0) {
        owner.mainChanged(id);
        oldOverride = clearOverrideSave();
    }
    if ((changed || panel.references(framePixels(retired))) &&
        !settingsPage.active && screenSlot(screenIndex) == frame.slot)
        show(screenIndex);
    retirePixels(framePixels(retired));
    retirePixels(framePixels(oldOverride));
    free(oldOverride);
    if (!refreshCache.staging) {  // the frame went into a released slot: a new scratch buffer
        refreshCache.staging = static_cast<uint8_t*>(ps_malloc(lcd::FRAME_BYTES));
        lcd::log("cache staging realloc ok=%d\n", refreshCache.staging != nullptr);
    }
    frameAccepted = true;
    xSemaphoreGive(frameCopied);
}

bool pendingCacheSave(uint32_t now) {
    if (deferredOverride.ready(now)) return true;
    for (int slot = 0; slot < lcd::SCREEN_SLOTS; ++slot)
        if ((slot != overrideSource || strcmp(persistedFrames[slot].id, deferredOverride.id)) && deferredFrames[slot].ready(now)) return true;
    return false;
}

bool persistDeferred(bool memoryPressure) {
    const uint32_t now = millis();

    // The spare backing a selected override must stay available until its
    // archive is committed. Other slots can save in either order.
    const auto due = [now, memoryPressure](const lcd::DeferredSave& save) {
        if (memoryPressure) return save.dueUnderPressure(now);
        return save.ready(now) && (!backlightState || now - save.queuedAt >= LitSaveIdleMs);
    };
    int slot = -1;
    const bool saveFrames = mounted && (!backlightState ||
        (litFrameSaveAllowed(now) && (memoryPressure || !refreshBusy || refreshStagingUpdate)));
    lcd::DeferredSave* chosen = saveFrames && due(deferredOverride) ? &deferredOverride : nullptr;
    if (saveFrames && !chosen) {
        for (int i = 0; i < lcd::SCREEN_SLOTS; ++i) {
            if ((i == overrideSource && !strcmp(persistedFrames[i].id, deferredOverride.id)) || !due(deferredFrames[i])) continue;
            if (!chosen || now - deferredFrames[i].queuedAt > now - chosen->queuedAt) {
                chosen = &deferredFrames[i];
                slot = i;
            }
        }
    }
    const bool saveSelection = !memoryPressure &&
        selectionSave.due(screenIndex, spareCursor, now, backlightState);
    if (!chosen && !saveSelection) return false;

    const bool wasLit = backlightState;
    ScanoutSavePause pause(saveSelection && !LitNvsWrites);
    if (saveSelection) {
        const bool saved = lcd::saveSelection(prefs, screenIndex, spareCursor);
        if (saved) selectionSave.restored(screenIndex, spareCursor);
        else selectionSave.failed(millis());
        lcd::log("selection persist screen=%d spare=%d ok=%d lit=%d\n",
                 screenIndex, spareCursor, saved, wasLit);
    }
    if (chosen) {
        const auto scanBefore = panel.health();
        const uint32_t started = millis();
        const size_t bytes = chosen->body.size;
        bool ok;
        if (slot < 0) {
            if (chosen->body.usable()) ok = lcd::writeCachedFrame("/override.vnr", chosen->body);
            else if (overrideSource >= 0 && overrideSource < lcd::SCREEN_SLOTS &&
                     lcd::overrideUsesResident(chosen->body, overrideSource,
                                               persistedFrames[overrideSource].id, chosen->id)) {
                const String source = lcd::residentPath(overrideSource, persistedFrames[overrideSource].alternate);
                ok = lcd::copyCachedFrame("/override.vnr", source.c_str());
            } else ok = false; // Keep the save queued until its source can be recovered.
            if (ok) overrideSource = -1;
        } else {
            ok = lcd::persistCacheSlot(prefs, slot, persistedFrames[slot], *chosen);
            if (ok && slot == 0) lcd::saveOwner(prefs, owner);
        }
        lcd::log("cache persist slot=%s id=%.16s bytes=%u ok=%d ms=%lu wait_ms=%lu lit=%d\n",
            slot < 0 ? "override" : lcd::SLOT_NAMES[slot], chosen->id, unsigned(bytes), ok,
            static_cast<unsigned long>(millis() - started),
            static_cast<unsigned long>(started - chosen->queuedAt), wasLit);
        if (wasLit) {
            const auto scanAfter = panel.health();
            lcd::log("cache persist scan restart=%u short=%u peak_us=%u max_gap_us=%u\n",
                     unsigned(scanAfter.lateRestarts - scanBefore.lateRestarts),
                     unsigned(scanAfter.shortFrames - scanBefore.shortFrames),
                     unsigned(scanAfter.maxFillUs), unsigned(scanAfter.maxVsyncGapUs));
        }
        if (ok) *chosen = {};
        else {
            chosen->failedAt = millis();
            chosen->retrying = true;
        }
    }
    return chosen != nullptr;
}

void reclaimCacheMemory(size_t bytes) {
    // Standalone frames can queue compressed PSRAM bodies until LittleFS
    // commits them. Host frames queue file leases instead.
    // ps_malloc requires byte-addressable PSRAM, even for RGB565 pixels.
    while (heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) < bytes) {
        if (!persistDeferred(true)) break;
    }
}

void resetUpdateDownload() {
    updateRetry = {};
    downloadStopped = installRequested = false;
    installQueued = lcd::UpdateSource::None;
}

bool forgetAcceptedUpdate() {
    ScanoutSavePause pause(!LitNvsWrites);
    if (!lcd::clearAcceptedUpdate(prefs, acceptedUpdate)) {
        updateNotice = "Could not save update choice";
        return false;
    }
    resetUpdateDownload();
    return true;
}

void reconcileUpdate(lcd::UpdateSource source, const lcd::UpdateOffer& current) {
    if (acceptedUpdate.source != source) return;
    const auto next = lcd::identifyUpdate(current, source);
    if (lcd::matchesAcceptance(acceptedUpdate, next)) return;
    ScanoutSavePause pause(!LitNvsWrites);
    if (lcd::reconcileAcceptedUpdate(prefs, acceptedUpdate, next)) {
        resetUpdateDownload();
        updateNotice = "Offer changed. Select the current update.";
    } else {
        updateNotice = "Could not save update choice";
        downloadStopped = true;
        installRequested = false;
        installQueued = lcd::UpdateSource::None;
    }
}

void sweepDownloads(uint32_t now, const lcd::AcceptedUpdate& accepted,
                    const lcd::DeviceManifest* manifest, const char* displayedArtId,
                    lcd::Watchdog& watchdog, bool boot = false) {
    if (now < resume_detail::kValidEpoch) return;
    const bool daily = boot || downloadSweepDay.due(now);
    const bool standalone = lcd::standalone(lcd::installation);
    const bool sweepFrames = manifest && frameSweepDay.due(now);
    if (!daily && !sweepFrames) return;
    lcd::DownloadSweep sweep;
    sweep.boot = boot;
    if (daily) {
        if (standalone && !boot)
            sweep.art = lcd::wantedArtTargets(now, displayedArtId);
        if (accepted.source != lcd::UpdateSource::None) {
            sweep.otaTarget = otaStageTarget(accepted.image);
            sweep.otaValidator = otaStageValidator(accepted.image);
            sweep.otaCommittedMatches = [&](FsStore& store, const char* target) {
                auto file = store.openCommitted(target);
                return file && verifyOtaFile(store, file, accepted.image,
                                          [&] { watchdog.feed(); }) == OtaFailure::None;
            };
        }
    }
    if (sweepFrames) {
        const auto publishedFrame = [manifest](const char* target) -> const lcd::FrameInfo* {
            for (int slot = 0; slot < lcd::SCREEN_SLOTS; ++slot) {
                const size_t length = strlen(lcd::SLOT_NAMES[slot]);
                if (strncmp(target + 6, lcd::SLOT_NAMES[slot], length) ||
                    (target[6 + length] && target[6 + length] != '.')) continue;
                if (slot >= 3 && slot - 3 >= manifest->spareCount) return nullptr;
                const auto& frame = slot < 3 ? manifest->screens[slot] : manifest->spares[slot - 3];
                return frame.present ? &frame : nullptr;
            }
            return nullptr;
        };
        sweep.frameLive = [publishedFrame](const char* target, const PartialMeta& meta) {
            const auto* frame = publishedFrame(target);
            return frame && frame->size == meta.total && !strcmp(frame->id, meta.validator);
        };
        sweep.frameCommittedWanted = [publishedFrame](FsStore& store, const char* target) {
            if (lcd::queuedFrameStage(store, target)) return true;
            const auto* frame = publishedFrame(target);
            const auto* version = strchr(target, '.');
            return frame && (!version || !strcmp(version + 1, frame->id));
        };
    }
    forEachPartialStore([&](FsStore& store) {
        watchdog.feed();
        sweep.run(store, now, daily);
    });
}

void refreshWorker(void*) {
    refreshWatchdog.subscribe();
    standaloneGithubAttempt = 0;
    if (refreshStagingUpdate) {
        stagedUpdateOutcome = lcd::stageUpdatePass(prefs, updateBoot, refreshAcceptedUpdate.offer,
            refreshAcceptedUpdate.source, refreshWatchdog, requestOtaSpareEviction,
            stagedUpdateFailure, countStagedUpdateFailure);
    } else if (refreshCheckingUpdates) {
        githubStatus = lcd::checkGithub(githubLastCheck, githubStaging, refreshWatchdog);
    } else if (lcd::standalone(lcd::installation)) {
        lcd::runLocalRefresh(refreshIds, refreshStaging, refreshWatchdog, refreshCache);
        if (refreshStaging.wifiOk && refreshStaging.manifestOk &&
            !lcd::anyRequiredFrameFailed(refreshStaging) && WiFi.status() == WL_CONNECTED) {
            const int64_t now = time(nullptr);
            const int64_t last = std::max(githubLastCheck, githubLastAutoFailure);
            if (lcd::githubCheckDue(true, last, now)) {
                standaloneGithubAttempt = now;
                githubStatus = lcd::checkGithub(last, githubStaging, refreshWatchdog);
            }
        }
    } else {
        lcd::runRefresh(lcd::installation.host, lcd::installation.profile, refreshIds,
                        refreshStaging, refreshWatchdog, refreshSettings, refreshSkips,
                        refreshCache, refreshOtaReport, refreshOtaApproval);
    }
    if (!refreshStagingUpdate && !refreshCheckingUpdates) {
        requestSweepContext();
        const auto epoch = ventanal::trustedEpoch(time(nullptr));
        const bool standalone = lcd::standalone(lcd::installation);
        const auto* manifest = refreshStaging.manifestOk && !standalone ? &refreshStaging.manifest : nullptr;
        const auto source = refreshAcceptedUpdate.source;
        const auto* current = source == lcd::UpdateSource::Host && manifest ? &refreshStaging.firmware
            : source == lcd::UpdateSource::GitHub && standaloneGithubAttempt && githubStaging.checked
                ? &githubStaging.offer : nullptr;
        if (current) {
            lcd::UpdateOffer offer;
            if (!lcd::validateOffer(*current, source, offer) ||
                !lcd::matchesAcceptance(refreshAcceptedUpdate, lcd::identifyUpdate(offer, source)))
                refreshAcceptedUpdate = {};
        }
        sweepDownloads(epoch, refreshAcceptedUpdate, manifest, sweepDisplayedArt, refreshWatchdog);
        if (refreshAcceptedUpdate.source != lcd::UpdateSource::None &&
            refreshAcceptedUpdate.source == refreshReadySource) {
            const auto target = otaStageTarget(refreshAcceptedUpdate.image);
            readyStageExpired = true;
            forEachPartialStore([&](FsStore& store) {
                if (store.openCommitted(target.c_str())) readyStageExpired = false;
            });
        }
    }
    refreshWatchdog.unsubscribe();
    xSemaphoreGive(refreshStaged);
    // Park until loop() has copied the result, so the staging buffer is
    // never read while the worker might still touch it.
    xSemaphoreTake(refreshCopied, portMAX_DELAY);
    vTaskDelete(nullptr);
}

// Kick a refresh on the worker task; loop() keeps servicing touch and the
// backlight while it runs. At most one worker may be in flight.
bool startRefresh(bool redraw, bool check = false, bool stageUpdate = false) {
    if (refreshBusy || setupActive || setupQueued) return false;
    if (!check && !stageUpdate && !refreshCache.staging) {  // a frame went into a lent slot and memory was short then
        refreshCache.staging = static_cast<uint8_t*>(ps_malloc(lcd::FRAME_BYTES));
        lcd::log("cache staging retry ok=%d\n", refreshCache.staging != nullptr);
    }
    refreshCheckingUpdates = check;
    refreshStagingUpdate = stageUpdate;
    if (stageUpdate) refreshAcceptedUpdate = acceptedUpdate;
    readyStageExpired = false;
    if (!check && !stageUpdate) {
        formatOtaReport(refreshOtaReport, updateBoot.journal.record);
        strlcpy(refreshOtaApproval, refreshOtaReport[0] ? updateBoot.journal.approval : "",
                sizeof refreshOtaApproval);
        memcpy(refreshIds, state.cachedIds, sizeof refreshIds);
        for (int slot = 0; slot < lcd::SCREEN_SLOTS; ++slot) {
            memcpy(refreshCache.persistedIds[slot], persistedFrames[slot].id,
                   sizeof refreshCache.persistedIds[slot]);
            const auto& save = deferredFrames[slot];
            refreshCache.retryFileSave[slot] = save.body.file && save.body.file->unavailable;
        }
#ifdef VENTANAL_LCD_DIAG_PANEL
        refreshCache.forceDownloads = fetchCounter == 0;
#ifdef VENTANAL_LCD_DIAG_F3
        refreshCache.forceDownloads = true;
#endif
#endif
        refreshCache.health = panel.health();
        refreshSettings = settings;
        owner.queue.query(refreshSkips,sizeof refreshSkips);
        sentSkipRevision=skipRevision;
        if(owner.queue.count) lcd::log("skip reports sending count=%u\n",unsigned(owner.queue.count));
        refreshRequested = redraw;
        lastFetchMs = millis();
    }
    refreshBusy = true;
    // Keep display drawing and the worker together. With the normal
    // same-core placement the RGB/GDMA ISRs preempt both PSRAM clients.
    const int workerCore = xPortGetCoreID();
    if (xTaskCreatePinnedToCore(refreshWorker, "ventanal-refresh", WORKER_STACK_BYTES,
                                nullptr, 1, nullptr, workerCore) != pdPASS) {
        refreshBusy = refreshCheckingUpdates = refreshStagingUpdate = false;
        lcd::log("refresh error=task-create\n");
        return false;
    }
    if (!check && !stageUpdate) settingsRefreshPending=false;
    return true;
}

// The caller handles scan-out before writing to flash.
void applyGithubCheck() {
    githubLastCheck = githubStaging.checked;
    lcd::validateOffer(githubStaging.offer, lcd::UpdateSource::GitHub, githubOffer);
    reconcileUpdate(lcd::UpdateSource::GitHub, githubOffer);
    if (!lcd::saveGithubCheck(prefs, githubStaging)) lcd::log("ota error=nvs-check\n");
}

// Apply a finished worker pass (single writer: loop()).
void applyFinishedRefresh() {
    if (refreshStagingUpdate) {
        const auto completed = stagedUpdateOutcome;
        if (acceptedUpdate.source != lcd::UpdateSource::None &&
            lcd::matchesAcceptance(acceptedUpdate, refreshAcceptedUpdate)) {
            if (completed == FetchOutcome::Complete) {
                installQueued = acceptedUpdate.source;
                updateNotice = "Ready to install. Tap Install.";
            } else if (completed == FetchOutcome::Failed) {
                ScanoutSavePause pause(!LitNvsWrites);
                const auto failure = lcd::recordUpdateFailure(prefs, updateBoot, acceptedUpdate.offer,
                    acceptedUpdate.source, stagedUpdateFailure, countStagedUpdateFailure);
                downloadStopped = true;
                updateNotice = failure == OtaFailure::Attempts
                    ? "Attempt limit reached for this update" : "Download failed. Tap Download to retry.";
                settingsRefreshPending = true;
            } else {
                if (completed == FetchOutcome::Progress) updateRetry = {};
                updateRetry.finished(millis(), false);
            }
        }
        xSemaphoreGive(refreshCopied);
        refreshBusy = refreshStagingUpdate = false;
        if (settingsPage.active && updatesPage) showSettings();
        backlightRefresh();
        return;
    }

    if (refreshCheckingUpdates) {
        if (githubStaging.checked) {
            ScanoutSavePause pause(!LitNvsWrites);
            applyGithubCheck();
        }
        switch (githubStatus) {
        case lcd::CheckResult::NotDue: updateNotice = "Checked within the last week"; break;
        case lcd::CheckResult::Available: updateNotice = ""; break;
        case lcd::CheckResult::UpToDate: updateNotice = "No compatible update found"; break;
        default: updateNotice = "Update check failed. Try again."; break;
        }
        xSemaphoreGive(refreshCopied);
        refreshBusy = refreshCheckingUpdates = false;
        if (settingsPage.active) showSettings();
        return;
    }

    const int previousIndex = screenIndex;

    // Did anything the screen shows change? frameOk slots that merely
    // re-stage the current id (skipped=unchanged) are not new content,
    // and a drop only counts when the slot had something to lose: solar
    // (and unpublished spares) are re-staged as drops on every pass.
    bool changed = false;
    if (refreshStaging.manifestOk) {
        for (int slot = 0; slot < lcd::SCREEN_SLOTS; ++slot) {
            // A corrupt cached file may have a stored id but no resident
            // frame to drop. Queue its deletion when it is unpublished.
            if (mounted && refreshStaging.dropSlot[slot] && persistedFrames[slot].id[0]
                && !deferredFrames[slot].pending) {
                deferredFrames[slot].replace("", {}, millis());
            }
            if ((refreshStaging.dropSlot[slot] && refreshIds[slot][0])
                || (refreshStaging.frameOk[slot]
                    && strcmp(refreshStaging.frameIds[slot],
                              refreshIds[slot]))) {
                changed = true;
            }
        }
    }
    wifiRetry.finished(millis(), refreshStaging.wifiOk);
    const unsigned before = owner.queue.count;
    lcd::acknowledgeSkips(owner, skipRevision - sentSkipRevision, refreshStaging.manifestOk);
    const bool writesOwner = before != owner.queue.count;
    const bool writesMetadata = refreshStaging.manifestOk &&
        lcd::manifestSaveNeeded(prefs, refreshStaging.manifest);
    const bool githubChecked = standaloneGithubAttempt && githubStaging.checked;
    const bool githubCheckFailed = standaloneGithubAttempt && githubStatus == lcd::CheckResult::Failed;
    const bool callStateChanged = refreshStaging.callCheckedAt &&
        refreshStaging.callState != lcd::installation.callState;
#ifdef VENTANAL_LCD_DIAG_STANDALONE_SAMPLE
    // The sample installation is a RAM-only override.
    const bool writesCallState = false;
#else
    const bool writesCallState = callStateChanged;
#endif
    // A failed check-in leaves the queued approval intact. A successfully
    // fetched manifest can replace or withdraw it.
    if (refreshStaging.manifestOk)
        lcd::validateOffer(refreshStaging.firmware, lcd::UpdateSource::Host, hostOffer);
    else if (acceptedUpdate.source != lcd::UpdateSource::Host)
        hostOffer = {};
    const bool staleRollback = lcd::rolledBackOffer(updateBoot, hostOffer);
    if (staleRollback) hostOffer = {};
    if (refreshStaging.manifestOk) reconcileUpdate(lcd::UpdateSource::Host, hostOffer);
    if (readyStageExpired && installQueued != lcd::UpdateSource::None) {
        installQueued = lcd::UpdateSource::None;
        installRequested = false;
        updateRetry = {};
        updateNotice = "Downloading update...";
    }
    const bool frameFailed = lcd::anyRequiredFrameFailed(refreshStaging);
    const bool incomplete = refreshStaging.framesPending || frameFailed;
    const uint32_t epochNow = ventanal::trustedEpoch(time(nullptr));
    if (!refreshStaging.memoryOnlyPending)
        contentStaleness.observe(epochNow, !refreshStaging.manifestOk || incomplete);
    const bool checkpoint = contentStaleness.checkpointDue(epochNow);
    bool redraw;
    {
        const bool ackUpdate = refreshStaging.manifestOk && refreshOtaReport[0] && !staleRollback;
        ScanoutSavePause pause(!LitNvsWrites && (writesMetadata || writesOwner || ackUpdate ||
                                                writesCallState || refreshStaging.artStateChanged ||
                                                githubChecked || githubCheckFailed || checkpoint));
        if (checkpoint) contentStaleness.checkpoint(prefs, epochNow);
        if (refreshStaging.artStateChanged && !lcd::saveArtState(refreshStaging.artState))
            lcd::log("standalone error=art-save\n");
        if (githubChecked) {
            applyGithubCheck();
        } else if (githubCheckFailed) {
            // Keep the cached offer and leave manual retries available.
            githubLastAutoFailure = standaloneGithubAttempt;
            if (!lcd::saveGithubAutoFailure(prefs, githubLastAutoFailure)) lcd::log("ota error=nvs-auto-check\n");
            lcd::log("ota github auto-check failed\n");
        }
        if (refreshStaging.callCheckedAt) {
            const uint8_t previousCallState = lcd::installation.callState;
            const uint32_t previousCallCheckedAt = lcd::installation.callCheckedAt;
            lcd::installation.callState = refreshStaging.callState;
            lcd::installation.callCheckedAt = refreshStaging.callCheckedAt;
            if (writesCallState && !lcd::saveInstallation(lcd::installation)) {
                lcd::installation.callState = previousCallState;
                lcd::installation.callCheckedAt = previousCallCheckedAt;
                lcd::log("standalone error=fcc-save\n");
            }
        }
        if (ackUpdate) lcd::acknowledgeUpdate(prefs, updateBoot, refreshOtaReport, refreshOtaApproval);
        redraw = lcd::applyRefresh(state, screenIndex, spareCursor, prefs, refreshStaging,
                                   refreshRequested, settings);
        if (refreshStaging.manifestOk) {
            lcd::log("skip reports acknowledged count=%u pending=%u\n",
                     before - owner.queue.count, unsigned(owner.queue.count));
            if (writesOwner) lcd::saveOwner(prefs, owner);
        }
    }
    refreshLabel.finish(millis(), !refreshStaging.manifestOk, changed, incomplete);
    const bool labelChanged = logRefreshLabel();
    lcd::log("refresh fetch=%lu ok=%d offline=%d failed=%d\n", fetchCounter++,
             refreshStaging.manifestOk, state.offline, frameFailed);
    xSemaphoreGive(refreshCopied);
    refreshBusy = false;
    if (screenIndex != previousIndex)   // normalised to a shrunk list
        lcd::log("screen=%s\n", lcd::SLOT_NAMES[screenSlot(screenIndex)]);
    if (redraw || labelChanged || ((callStateChanged || githubChecked) && settingsPage.active))
        show(screenIndex);
    backlightRefresh();
    lastFetchCompletedMs = millis();
}
