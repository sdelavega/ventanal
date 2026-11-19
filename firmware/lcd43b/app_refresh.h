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
lcd::RefreshResult refreshStaging;   // worker writes, loop copies while parked
char refreshIds[lcd::SCREEN_SLOTS][17]{};
struct PendingFrame {
    int slot;
    const lcd::FrameInfo* info;
    const lcd::FrameBody* body;
    size_t requestedBytes = 0;
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
    if (frame.slot < 0) {
        if (!backlightState || litFrameSaveAllowed(millis())) reclaimCacheMemory(frame.requestedBytes);
        xSemaphoreGive(frameCopied);
        return;
    }
    uint8_t* retired = nullptr;
    if (frame.info) {
        const bool pinnedSpare = residentFrames[frame.slot] == overrideFrame;
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
    auto& id = state.cachedIds[frame.slot];
    const char* nextId = frame.info ? frame.info->id : "";
    const bool changed = strcmp(id, nextId) != 0;
    if (mounted && changed) {
        auto& save = deferredFrames[frame.slot];
        if (save.replace(nextId, *frame.body, millis())) {
            lcd::log("cache queued slot=%s bytes=%u free_psram=%u\n", lcd::SLOT_NAMES[frame.slot],
                     unsigned(frame.body->size), unsigned(ESP.getFreePsram()));
        }
    }
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
    frameAccepted = true;
    xSemaphoreGive(frameCopied);
}

bool pendingCacheSave(uint32_t now) {
    if (deferredOverride.ready(now)) return true;
    for (int slot = 0; slot < lcd::SCREEN_SLOTS; ++slot)
        if (slot != overrideSource && deferredFrames[slot].ready(now)) return true;
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
        (litFrameSaveAllowed(now) && (memoryPressure || !refreshBusy)));
    lcd::DeferredSave* chosen = saveFrames && due(deferredOverride) ? &deferredOverride : nullptr;
    if (saveFrames && !chosen) {
        for (int i = 0; i < lcd::SCREEN_SLOTS; ++i) {
            if (i == overrideSource || !due(deferredFrames[i])) continue;
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
            if (chosen->body.bytes) ok = lcd::writeCachedFrame("/override.vnr", chosen->body);
            else {
                const String source = lcd::residentPath(overrideSource, persistedFrames[overrideSource].alternate);
                ok = lcd::copyCachedFrame("/override.vnr", source.c_str());
            }
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
    // Queued frames own their compressed bodies until LittleFS commits them.
    // A cold cache can fill PSRAM before the regular dark-screen save window.
    // ps_malloc requires byte-addressable PSRAM, even for RGB565 pixels.
    while (heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) < bytes) {
        if (!persistDeferred(true)) break;
    }
}

void refreshWorker(void*) {
    refreshWatchdog.subscribe();
    if (refreshCheckingUpdates) {
        githubStatus = lcd::checkGithub(githubLastCheck, githubStaging, refreshWatchdog);
    } else {
        lcd::runRefresh(lcd::installation.host, lcd::installation.profile, refreshIds,
                        refreshStaging, refreshWatchdog, refreshSettings, refreshSkips,
                        refreshCache, refreshOtaReport, refreshOtaApproval);
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
bool startRefresh(bool redraw, bool check = false) {
    if (refreshBusy || setupActive || setupQueued) return false;
    refreshCheckingUpdates = check;
    if (!check) {
        formatOtaReport(refreshOtaReport, updateBoot.journal.record);
        strlcpy(refreshOtaApproval, refreshOtaReport[0] ? updateBoot.journal.approval : "",
                sizeof refreshOtaApproval);
        memcpy(refreshIds, state.cachedIds, sizeof refreshIds);
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
        refreshBusy = refreshCheckingUpdates = false;
        lcd::log("refresh error=task-create\n");
        return false;
    }
    if (!check) settingsRefreshPending=false;
    return true;
}

// Apply a finished worker pass (single writer: loop()).
void applyFinishedRefresh() {
    if (refreshCheckingUpdates) {
        if (githubStaging.checked) {
            ScanoutSavePause pause(!LitNvsWrites);
            githubLastCheck = githubStaging.checked;
            lcd::validateOffer(githubStaging.offer, lcd::UpdateSource::GitHub, githubOffer);
            if (!lcd::saveGithubCheck(prefs, githubStaging)) lcd::log("ota error=nvs-check\n");
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
    if (!refreshStaging.manifestOk
        || !lcd::validateOffer(refreshStaging.firmware, lcd::UpdateSource::Host, hostOffer))
        hostOffer = {};
    const bool staleRollback = lcd::rolledBackOffer(updateBoot, hostOffer);
    if (staleRollback) hostOffer = {};
    bool redraw;
    {
        const bool ackUpdate = refreshStaging.manifestOk && refreshOtaReport[0] && !staleRollback;
        ScanoutSavePause pause(!LitNvsWrites && (writesMetadata || writesOwner || ackUpdate));
        if (ackUpdate) lcd::acknowledgeUpdate(prefs, updateBoot, refreshOtaReport, refreshOtaApproval);
        redraw = lcd::applyRefresh(state, screenIndex, spareCursor, prefs, refreshStaging,
                                   refreshRequested, settings);
        if (refreshStaging.manifestOk) {
            lcd::log("skip reports acknowledged count=%u pending=%u\n",
                     before - owner.queue.count, unsigned(owner.queue.count));
            if (writesOwner) lcd::saveOwner(prefs, owner);
        }
    }
    xSemaphoreGive(refreshCopied);
    refreshBusy = false;
    // A manifest that arrived but lost a frame it required is a failure for
    // feedback: DOWNLOAD FAILED (red), never UP TO DATE over stale artwork.
    const bool frameFailed = lcd::anyRequiredFrameFailed(refreshStaging);
    if (refreshRequested)
        refreshLabel.finish(millis(), !refreshStaging.manifestOk, changed,
                            frameFailed);
    else if (redraw)
        refreshLabel.clear();   // a scheduled redraw replaces the held label
    logRefreshLabel();
    lcd::log("refresh fetch=%lu ok=%d offline=%d failed=%d\n", fetchCounter++,
             refreshStaging.manifestOk, state.offline, frameFailed);
    if (screenIndex != previousIndex)   // normalised to a shrunk list
        lcd::log("screen=%s\n", lcd::SLOT_NAMES[screenSlot(screenIndex)]);
    if (redraw) show(screenIndex);
    else if (refreshLabel.label() != lcd::RefreshLabel::None)
        show(screenIndex);   // redraw the cached frame with the label
    backlightRefresh();
}
