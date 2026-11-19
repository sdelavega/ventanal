// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
//
// Settings changes, Settings taps, gestures and touch polling.
//
// Part of lcd43b.ino's single translation unit: included only by lcd43b.ino,
// inside its anonymous namespace, in the order listed there. Earlier parts
// provide the declarations later parts use. Do not include it anywhere else.
#pragma once
// Task 6 calls this from the UI thread when a settings choice changes.
// An in-flight worker finishes its immutable snapshot; the next pass uses the
// new visibility so newly enabled screens get downloaded even after a toggle.
bool applyDeviceSettings(const lcd::DeviceSettings& next, lcd::SettingsHit hit) {
    if(!lcd::validSettings(next)) return false;
    const int previous=screenSlot(screenIndex);
    const bool visibilityChanged=next.showArt!=settings.showArt || next.showWeather!=settings.showWeather
        || next.showSolar!=settings.showSolar;
#ifndef VENTANAL_LCD_DIAG_DIM
    settingsDirty = installationReady;
#endif
    const bool brightnessChanged = settings.brightness != next.brightness;
    settings=next;
    lcd::remapSelection(screenIndex,spareCursor,previous,solarPublished(),spareCount(),settings);
    showSettings(brightnessChanged ? lcd::ReadoutRect{} : lcd::settingsDamage(hit));
    if(visibilityChanged) settingsRefreshPending=true;
    return true;
}

// --- Touch ----------------------------------------------------------------

void closeSettings(const char* reason) {
    settingsPage.active=false;
    updatesPage=false;
    touchPoller.controlsMode(false);
    lcd::remapSelection(screenIndex,spareCursor,settingsReturnSlot,solarPublished(),spareCount(),settings);
    lcd::log("settings close=%s return=%s\n",reason,lcd::SLOT_NAMES[screenSlot(screenIndex)]);
    show(screenIndex);
}
void queueInstall(lcd::UpdateSource source, const lcd::UpdateOffer& offer) {
    queuedOffer = offer;
    installQueued = source;
    lcd::log("ota install=queued refresh_busy=%d\n", refreshBusy);
    if (refreshBusy) showSettings();
}

void settingsTap(int x,int y) {
    if (updatesPage) {
        const bool busy = checkQueued || refreshCheckingUpdates || installQueued != lcd::UpdateSource::None;
        const bool waiting = installQueued != lcd::UpdateSource::None;
        switch (lcd::hitUpdates(x, y, hostOffer.version[0], githubOffer.version[0], busy, waiting)) {
        case lcd::UpdateHit::Cancel:
            installQueued = lcd::UpdateSource::None;
            lcd::log("ota install=cancelled\n");
            break;
        case lcd::UpdateHit::Later:
            updatesPage = false;
            break;
        case lcd::UpdateHit::Check:
            checkQueued = true;
            updateNotice = "Checking for updates...";
            break;
        case lcd::UpdateHit::HostInstall:
            queueInstall(lcd::UpdateSource::Host, hostOffer);
            return;
        case lcd::UpdateHit::GithubInstall:
            queueInstall(lcd::UpdateSource::GitHub, githubOffer);
            return;
        default:
            return;
        }
        showSettings();
        return;
    }
    if (!updateBoot.pending && lcd::inUpdateControl(lcd::UPDATE_MENU, x, y)) {
        updatesPage = true;
        showSettings();
        return;
    }
    const auto hit=lcd::hitSettings(x,y,solarPublished(),settingsBatteryButton());
    lcd::log("settings hit=%d x=%d y=%d\n",int(hit),x,y);
    if(hit==lcd::SettingsHit::Done) { closeSettings("done"); return; }
    if(hit==lcd::SettingsHit::SetupWifi) { enterSetup(lcd::SetupReason::UserRequested); return; }
    auto next=settings;
    const bool accepted=lcd::applySettingsHit(next,hit,solarPublished(),settingsBatteryMv());
    if(accepted && applyDeviceSettings(next, hit)) {
        lcd::log("settings change=%d brightness=%u timeout=%u art=%d weather=%d solar=%d queued=1\n",
                 int(hit),settings.brightness,settings.timeoutMin,settings.showArt,settings.showWeather,settings.showSolar);
    } else lcd::log("settings change=%d accepted=0\n",int(hit));
}

void handleGesture(Gesture gesture, int x, int y) {
    if (setupActive) {
        if (gesture == Gesture::Next || gesture == Gesture::Previous) setupTap(x, y);
        return;
    }
    if(settingsPage.active) {
        if(gesture==Gesture::Next || gesture==Gesture::Previous) settingsTap(x,y);
        return;
    }
    if((gesture==Gesture::Next || gesture==Gesture::Previous)
       && classifyTap(x,y,lcd::isLcarsSlot(screenSlot(screenIndex)))==TapTarget::OpenSettings) {
        settingsReturnSlot=screenSlot(screenIndex);
        settingsPage.open(millis());
        powerSession.activity(millis());
        touchPoller.controlsMode(true);
        refreshLabel.clear();
        lcd::log("settings open return=%s x=%d y=%d\n",lcd::SLOT_NAMES[settingsReturnSlot],x,y);
        if(!showSettings() && settingsPage.active) closeSettings("error");
        return;
    }
    const char* name = gesture == Gesture::Next      ? "next"
                     : gesture == Gesture::Previous  ? "previous"
                     : gesture == Gesture::DoubleTap ? "double"
                                                      : "refresh";
    lcd::log("gesture=%s x=%d y=%d\n", name, x, y);
    const int count = screenCount();
    switch (gesture) {
    case Gesture::Next:
        screenIndex = (screenIndex + 1) % count;
        lcd::log("screen=%s\n", lcd::SLOT_NAMES[screenSlot(screenIndex)]);
        show(screenIndex);
        break;
    case Gesture::Previous:
        screenIndex = (screenIndex + count - 1) % count;
        lcd::log("screen=%s\n", lcd::SLOT_NAMES[screenSlot(screenIndex)]);
        show(screenIndex);
        break;
    case Gesture::DoubleTap:
        {
            if (!mounted) { lcd::log("spare skipped=littlefs-unmounted\n"); break; }
            if(!settings.showArt) { lcd::log("spare skipped=art-hidden\n"); break; }
            bool cached[3];
            for(int i=0;i<3;++i) {
                strcpy(owner.spares[i].id,state.cachedIds[3+i]);
                cached[i]=owner.spares[i].id[0];
            }
            char skipped[17]; strcpy(skipped,lcd::selectedArtId(owner));
            const bool swapped=lcd::swapArt(owner,settings.showArt,cached,[](int next) {
                return residentFrames[3 + next] != nullptr;
            });
            if (!swapped) {
                lcd::log("spare skipped=no-psram\n");
                break;
            }
            uint8_t* oldOverride = nullptr;
            if (owner.cursor < 0) oldOverride = clearOverrideSave();
            else {
                oldOverride = ownOverrideFrame ? overrideFrame : nullptr;
                const int slot = 3 + owner.cursor;
                overrideFrame = residentFrames[slot];
                ownOverrideFrame = false;
                const auto& save = deferredFrames[slot];
                const bool inRam = save.pending && save.body.bytes;
                overrideSource = inRam ? -1 : slot;
                deferredOverride.replace(owner.overrideArt.id, inRam ? save.body : lcd::FrameBody{}, millis());
            }
            if(strlen(skipped)==16 && strlen(lcd::selectedArtId(owner))==16
               && strcmp(skipped,lcd::selectedArtId(owner))) ++skipRevision;
            spareCursor=owner.cursor;
            screenIndex=0;
            ScanoutSavePause pause(!LitNvsWrites);
            lcd::saveOwner(prefs,owner);
            lcd::log("spare selected=%d id=%s queued=%u screen=art\n",owner.cursor,lcd::selectedArtId(owner),unsigned(owner.queue.count));
            show(screenIndex);
            retirePixels(framePixels(oldOverride));
            free(oldOverride);
        }
        break;
    case Gesture::Refresh:
        // Non-blocking: the worker refreshes while touch keeps working,
        // and the label shows immediately that the gesture registered.
        refreshLabel.begin(millis());
        logRefreshLabel();
        if (!startRefresh(true)) {
            if (refreshBusy)
                refreshRequested = true;   // adopt the in-flight pass
            else
                refreshLabel.clear();       // nothing will finish it
        }
        show(screenIndex);             // redraw the frame with the label
        break;
    default:
        break;
    }
    backlightRefresh();
}

void restoreTouchWake();
void pollTouch(uint32_t now) {
    if(!powerSession.touchReady) return;
    if (now - lastTouchPollMs < TOUCH_POLL_MS) return;
    lastTouchPollMs = now;
    Gesture gesture = Gesture::None;
    bool touched = false;
    int x = 0, y = 0;
#ifdef VENTANAL_LCD_DIAG_GESTURES
    if (diagActive && diagStartMs && now >= diagStartMs) {
        // Synthetic samples through the real classifier (see diag.h);
        // real touch takes over again once the sequence completes.
        bool synthetic = false;
        if (!lcd::diagTouchSample(now - diagStartMs, synthetic, x, y)) {
            diagActive = false;
            touched = false;
            lcd::log("diag gestures=done\n");
        } else {
            gesture = touchPoller.sampleSynthetic(now, synthetic, x, y);
            touched = synthetic;
        }
    } else {
        lcd::TouchSample sample;
        gesture = touchPoller.poll(now, &sample);
        touched = sample.contact;
        x = sample.x;
        y = sample.y;
    }
#else
    lcd::TouchSample sample;
    gesture = touchPoller.poll(now, &sample);
    touched = sample.contact;
    x = sample.x;
    y = sample.y;
#endif
#ifdef VENTANAL_LCD_DIAG_TOUCH
    // Every report change, plus at most one line per second while held.
    // The CH422G shadow proves no later expander write re-asserted
    // TP_RST (bit 1) while touch was live.
    if (sample.report != lastRawSample.report
        || now - lastRawLogMs >= 1000) {
        lcd::log("touch raw=%d status=0x%02x contact=%d x=%d y=%d int=%d"
                 " ch422g_io=0x%02x\n",
                 int(sample.report), sample.status, sample.contact, x, y,
                 digitalRead(4), expander.outputs());
        lastRawLogMs = now;
    }
    lastRawSample = sample;
#endif
    if(touched) selectionSave.activity(now);
    if(touched && !powerSession.screenOn) {
        restoreTouchWake(); // a contact during a background download also wakes
        return;
    }
    if(touched) powerSession.activity(now);
    if(touched && settingsPage.active) settingsPage.touch(now);
    if (touched && !lastTouched) touchUntilMs = now + TOUCH_WAKE_MS;
    lastTouched = touched;
    if (gesture != Gesture::None && !powerSession.chargeShowing) handleGesture(gesture, touchPoller.pressX(), touchPoller.pressY());
}
