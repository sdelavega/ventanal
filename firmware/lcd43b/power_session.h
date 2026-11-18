// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
#pragma once
#include "power.h"
namespace lcd {
enum class PowerStep { None, ShowCharge, Restore, Sleep };
struct PowerSession {
    bool screenOn=true;
    bool timerRefreshPending=false;
    bool chargeShowing=false;
    bool touchReady=true;
    uint32_t lastActivity=0, chargeSince=0, touchProbeMs=0;
    uint32_t probeBackoffMs=1000;
    void activity(uint32_t now) { lastActivity=now; }
    uint32_t idle(uint32_t now) const { return now-lastActivity; }
    void sleep() { screenOn=false; }
    void wakeTouch(uint32_t now) { screenOn=true; activity(now); }
    void wakeTimer() { timerRefreshPending=true; } // keep panel dark
    bool refreshDue(uint32_t now,uint32_t lastFetch,uint32_t interval,
                    BatteryLevel level,bool busy,bool settingsPending) const {
        return !busy && level!=BatteryLevel::Critical &&
            (timerRefreshPending || settingsPending || (interval && now-lastFetch>=interval));
    }
    void refreshStarted(bool success) { if(success) timerRefreshPending=false; }
    void showCharge(uint32_t now) { screenOn=true; chargeShowing=true; chargeSince=now; }
    // A failed critical-screen resume converges to touch-only sleep next tick.
    void chargeFailed(uint32_t now) { showCharge(now); chargeSince=now-10000; }
    PowerStep step(PowerAction action,uint32_t now,bool busy) {
        if(action!=PowerAction::ChargeMe && chargeShowing) {
            chargeShowing=false; return PowerStep::Restore;
        }
        if(action==PowerAction::ChargeMe) timerRefreshPending=false;
        if(busy || timerRefreshPending) return PowerStep::None;
        if(action==PowerAction::ChargeMe) {
            if(!chargeShowing) return PowerStep::ShowCharge;
            if(now-chargeSince<10000) return PowerStep::None;
            chargeShowing=false; return PowerStep::Sleep;
        }
        return !screenOn || action==PowerAction::Sleep ? PowerStep::Sleep : PowerStep::None;
    }
    bool probeDue(uint32_t now) const { return !touchReady && now-touchProbeMs>=probeBackoffMs; }
    void probed(bool ok,uint32_t now) {
        touchReady=ok; touchProbeMs=now;
        probeBackoffMs=ok?1000:(probeBackoffMs>=30000?60000:probeBackoffMs*2);
    }
};
// No USB change on arming errors. A rejected sleep must undo the pre-sleep
// detach; only a successful return permits wake-cause handling at the caller.
template<class Sleep,class Pads>
int sleepWithUsb(int armed,Sleep sleep,Pads pads) {
    if(armed!=0) return armed;
    pads(false);
    const int result=sleep();
    pads(true); // resume on success; rollback on rejection
    return result;
}
} // namespace lcd
