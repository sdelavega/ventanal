// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega

// The display owner decides when the backlight may be lit. Pinned here:
// the policy matches the historical backlightRefresh() formula exactly,
// dark leases are counted per reason, and the screen only relights after
// the last lease ends and the adapter reports a settled frame.

#include "../lcd43b/display_owner.h"

#include <cassert>
#include <cstdio>
#include <cstring>

namespace {

using lcd::DarkReason;
using lcd::DisplayInputs;
using lcd::DisplayOwner;

DisplayInputs lit() {
    DisplayInputs in{};
    in.panelRunning = true;
    in.screenOn = true;
    in.now = 1000;
    in.touchWakeUntil = 0;
    return in;
}

// The formula the firmware used before the owner existed.
bool historical(const DisplayInputs& in) {
    if (!in.screenOn) return false;
    if (in.chargeShowing) return in.panelRunning;
    const bool quietBlanking = in.quiet && !in.installQueued && in.now >= in.touchWakeUntil;
    return !quietBlanking && in.panelRunning;
}

}  // namespace

int main() {
    {   // Exhaustive equivalence with the historical policy when no lease is held.
        DisplayOwner owner;
        for (unsigned bits = 0; bits < 64; ++bits) {
            DisplayInputs in{};
            in.panelRunning = bits & 1;
            in.screenOn = bits & 2;
            in.chargeShowing = bits & 4;
            in.quiet = bits & 8;
            in.installQueued = bits & 16;
            in.now = 5000;
            in.touchWakeUntil = (bits & 32) ? 9000 : 0;
            assert(owner.policyLit(in) == historical(in));
            assert(owner.decide(in, true).lit == historical(in));
        }
    }
    {   // Quiet blanking is reported separately (the firmware publishes it).
        DisplayOwner owner;
        auto in = lit();
        in.quiet = true;
        assert(owner.quietBlanking(in));
        in.touchWakeUntil = 2000;
        assert(!owner.quietBlanking(in));
        in.touchWakeUntil = 0;
        in.installQueued = true;
        assert(!owner.quietBlanking(in));
        in.installQueued = false;
        in.screenOn = false;
        assert(!owner.quietBlanking(in));  // historical: reset before the screen-off return
    }
    {   // A lease keeps the screen dark; nested leases count per reason.
        DisplayOwner owner;
        const auto in = lit();
        assert(owner.decide(in, true).lit);
        owner.acquire(DarkReason::Save);
        owner.acquire(DarkReason::Save);
        owner.acquire(DarkReason::Retire);
        auto d = owner.decide(in, true);
        assert(!d.lit && d.state == DisplayOwner::State::Dark);
        assert(std::strcmp(d.reason, "lease:save") == 0);
        assert(owner.held(DarkReason::Save) == 2 && owner.held(DarkReason::Retire) == 1);
        owner.release(DarkReason::Save);
        owner.release(DarkReason::Retire);
        assert(!owner.decide(in, true).lit);
        owner.release(DarkReason::Save);
        assert(!owner.anyLease());
    }
    {   // After the last lease the screen relights only once the frame settled.
        DisplayOwner owner;
        const auto in = lit();
        owner.acquire(DarkReason::Update);
        owner.release(DarkReason::Update);
        auto d = owner.decide(in, false);
        assert(!d.lit && d.state == DisplayOwner::State::Relighting);
        assert(std::strcmp(d.reason, "relight") == 0);
        d = owner.decide(in, true);
        assert(d.lit && d.state == DisplayOwner::State::Lit);
        d = owner.decide(in, false);   // once lit, later frames do not gate it
        assert(d.lit);
    }
    {   // Relighting is cancelled when the policy says dark anyway.
        DisplayOwner owner;
        auto in = lit();
        owner.acquire(DarkReason::Sleep);
        owner.release(DarkReason::Sleep);
        in.screenOn = false;
        auto d = owner.decide(in, false);
        assert(!d.lit && d.state == DisplayOwner::State::Dark);
        assert(std::strcmp(d.reason, "screen-off") == 0);
    }
    {   // A stopped panel is Off, whatever else holds.
        DisplayOwner owner;
        auto in = lit();
        in.panelRunning = false;
        const auto d = owner.decide(in, true);
        assert(!d.lit && d.state == DisplayOwner::State::Off);
        assert(std::strcmp(d.reason, "panel-stopped") == 0);
    }
    {   // Releasing a lease that is not held is ignored, never underflows.
        DisplayOwner owner;
        owner.release(DarkReason::Diag);
        assert(owner.held(DarkReason::Diag) == 0 && !owner.anyLease());
    }
    {   // Long-held leases are reported (except update and sleep).
        DisplayOwner owner;
        owner.acquire(DarkReason::Save, 100);
        assert(!owner.overdue(100 + 9999));
        assert(owner.overdue(100 + 10000));
        owner.release(DarkReason::Save);
        owner.acquire(DarkReason::Update, 100);
        assert(!owner.overdue(100 + 60000));
    }
    {   // The scoped lease releases on every exit path.
        DisplayOwner owner;
        {
            lcd::DarkLease lease(owner, DarkReason::Retire);
            assert(owner.held(DarkReason::Retire) == 1);
        }
        assert(owner.held(DarkReason::Retire) == 0);
        {
            lcd::DarkLease lease(owner, DarkReason::Save, false);  // not needed: no-op
            assert(!owner.anyLease());
        }
    }
    {   // Reason names for logging.
        assert(std::strcmp(lcd::darkReasonName(DarkReason::Save), "save") == 0);
        assert(std::strcmp(lcd::darkReasonName(DarkReason::Update), "update") == 0);
    }
    std::puts("lcd43b display owner: historical policy equivalence, leases, relight, off state, overdue leases passed");
}
