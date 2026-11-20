// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
//
// Diagnostic-build probes for lit writes and busy installs.
//
// Part of lcd43b.ino's single translation unit: included only by lcd43b.ino,
// inside its anonymous namespace, in the order listed there. Earlier parts
// provide the declarations later parts use. Do not include it anywhere else.
#pragma once
#ifdef VENTANAL_LCD_DIAG_F3
void probeLitWrites(uint32_t now) {
    if (stressProfile != 2 || stressSettingsCycles < 20 || writeProbeCount >= 60 || !canvas || !mounted || !backlightState
        || now - writeProbeAt < 2250 || refreshBusy) return;
    writeProbeAt = now;
    const auto before = panel.health();
    ++writeProbeCount;
    bool nvsOk;
    {
        lcd::ScanScope writing(lcd::ScanActivity::Nvs);
        nvsOk = prefs.putUInt("f3-write-probe", writeProbeCount) == sizeof(uint32_t);
    }
    bool fsOk = true;
    {
        lcd::ScanScope writing(lcd::ScanActivity::LittleFS);
        memset(canvas, 0xA5, 1024);
        File output = LittleFS.open("/f3-probe.tmp", "w");
        if (!output) fsOk = false;
        else {
            for (int size = 0; size < 64 * 1024; size += 1024) {
                if (output.write(reinterpret_cast<uint8_t*>(canvas), 1024) != 1024) {
                    fsOk = false;
                    break;
                }
            }
            output.close();
        }
        fsOk = LittleFS.remove("/f3-probe.tmp") && fsOk;
    }
    if (writeProbeCount == 60) prefs.remove("f3-write-probe");
    const auto after = panel.health();
    lcd::log("diag lit-write seq=%u nvs=%d fs=%d restart=%u short=%u peak_us=%u max_gap_us=%u\n",
             writeProbeCount, nvsOk, fsOk, unsigned(after.lateRestarts - before.lateRestarts),
             unsigned(after.shortFrames - before.shortFrames), unsigned(after.maxFillUs),
             unsigned(after.maxVsyncGapUs));
}
#endif

#if defined(VENTANAL_LCD_DIAG_F3) && defined(VENTANAL_LCD_DIAG_INSTALL_VERSION)
void probeBusyInstall() {
    static unsigned phase = 0;
#ifdef VENTANAL_LCD_DIAG_DIM
    const bool ready = dimStep == 100;
#else
    const bool ready = stressProfile == 2 && stressCycles == 90 && writeProbeCount == 60;
#endif
    if (phase == 2 && installQueued == lcd::UpdateSource::Host && !refreshBusy) {
        selectUpdate(lcd::UpdateSource::Host, hostOffer);
        ++phase;
        return;
    }
    if (phase >= 2 || !ready || strcmp(hostOffer.version, VENTANAL_LCD_DIAG_INSTALL_VERSION)
        || refreshBusy || !backlightState) return;
    if (phase == 0 && !startRefresh(true)) return;
    handleGesture(Gesture::Next, 600, 20);
    handleGesture(Gesture::Next, 500, 55);
    handleGesture(Gesture::Next, 650, 138);
    if (phase == 0) {
        const bool waitingLit = refreshBusy && backlightState && acceptedUpdate.source == lcd::UpdateSource::Host;
        handleGesture(Gesture::Next, 660, 411);
        const bool cancelled = backlightState && acceptedUpdate.source == lcd::UpdateSource::None;
        lcd::log("diag ota waiting_lit=%d cancelled_lit=%d refresh_busy=%d\n",
                 waitingLit, cancelled, refreshBusy);
        if (!cancelled || !waitingLit) {
            forgetAcceptedUpdate();
            phase = 2;
        }
        closeSettings("diag");
    }
    ++phase;
}
#endif
