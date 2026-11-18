// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
#include "../lcd43b/power_session.h"
#include <cassert>
#include <cstdio>
#include <vector>
using namespace lcd;
int main() {
    {
        lcd::PowerSession failed;
        failed.chargeFailed(7); // also exercises unsigned millis wrap
        assert(failed.step(lcd::PowerAction::ChargeMe,8,false)==lcd::PowerStep::Sleep);
        failed.sleep();
        // Sleep returns only on touch; the next attempt is then explicitly requested.
        failed.wakeTouch(100);
        assert(failed.step(lcd::PowerAction::ChargeMe,100,false)==lcd::PowerStep::ShowCharge);
    }

    PowerSession s;
    s.activity(0); s.sleep(); s.wakeTimer();
    // Overdue timer wakes must start background work, never re-sleep first.
    assert(!s.screenOn && s.timerRefreshPending);
    assert(s.refreshDue(2000000,0,1800000,BatteryLevel::None,false,false));
    assert(pollWaitMs(2000000,1800000)==1);
    assert(s.step(PowerAction::Sleep,2000000,false)==PowerStep::None);
    s.refreshStarted(false);
    assert(s.timerRefreshPending);
    assert(s.refreshDue(2000001,2000000,1800000,BatteryLevel::None,false,false));
    s.refreshStarted(true);
    assert(!s.timerRefreshPending);
    assert(s.step(PowerAction::Sleep,2000002,true)==PowerStep::None);
    // Contact while the worker downloads restores display and its idle timer.
    s.wakeTouch(2000003);
    assert(s.screenOn && s.idle(2000003)==0);
    assert(s.step(PowerAction::StayOn,2000004,true)==PowerStep::None);
    assert(s.step(PowerAction::StayOn,2000005,false)==PowerStep::None);
    // Worker completion while dark permits sleep only after work is applied.
    s.sleep(); s.wakeTimer(); s.refreshStarted(true);
    assert(s.step(PowerAction::StayOn,2000010,true)==PowerStep::None);
    assert(!s.screenOn);
    assert(s.step(PowerAction::StayOn,2000011,false)==PowerStep::Sleep);
    assert(!s.screenOn);
    // Settings refresh and timer refresh are independent pending triggers.
    assert(s.refreshDue(1,0,1800000,BatteryLevel::None,false,true));
    assert(!s.refreshDue(1,0,1800000,BatteryLevel::None,true,true));
    // Critical transition from dark shows a full countdown, including wrap.
    s.wakeTimer();
    assert(s.step(PowerAction::ChargeMe,0,true)==PowerStep::None);
    assert(!s.timerRefreshPending);
    assert(s.step(PowerAction::ChargeMe,0,false)==PowerStep::ShowCharge);
    s.showCharge(UINT32_MAX-4999);
    assert(s.screenOn && s.chargeShowing);
    assert(s.step(PowerAction::ChargeMe,4999,false)==PowerStep::None);
    assert(s.step(PowerAction::ChargeMe,5000,true)==PowerStep::None);
    assert(s.step(PowerAction::ChargeMe,5000,false)==PowerStep::Sleep);
    assert(pollIntervalMs(false,BatteryLevel::Critical)==0);
    s.sleep(); s.wakeTouch(6000);
    assert(s.step(PowerAction::ChargeMe,6000,false)==PowerStep::ShowCharge);
    s.showCharge(6000);
    assert(s.step(PowerAction::StayOn,6001,false)==PowerStep::Restore);
    assert(!s.chargeShowing);
    // Failed GT911 probes back off across wakes, cap at 60 s, recover.
    uint32_t now=10000;
    for(int i=0;i<10;++i) {
        s.probed(false,now); s.sleep(); s.wakeTimer();
        assert(s.probeBackoffMs<=60000);
        assert(!s.probeDue(now+s.probeBackoffMs-1));
        now+=s.probeBackoffMs;
        assert(s.probeDue(now));
    }
    assert(s.probeBackoffMs==60000);
    s.probed(true,now); assert(s.touchReady && s.probeBackoffMs==1000);
    // USB order: arming error does nothing; rejection rolls detach back.
    for(int result: {0,7}) {
        std::vector<int> events;
        assert(sleepWithUsb(0,[&] { events.push_back(2); return result; },
            [&](bool on) { events.push_back(on?3:1); })==result);
        assert((events==std::vector<int>{1,2,3}));
    }
    assert(sleepWithUsb(9,[] { assert(false); return 0; },[](bool) { assert(false); })==9);
    puts("lcd43b power session: overdue timer, background touch/completion, critical countdown/recovery, busy deferral, failed start, probe backoff, USB ordering passed");
}
