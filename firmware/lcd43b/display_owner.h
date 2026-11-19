// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
#pragma once

// The single owner of "may the screen be lit?". Pure logic, no hardware:
// the firmware's adapter (app_display.h) feeds it inputs, applies the
// decision through the CH422G, and is the only code allowed to switch the
// backlight. Code that needs the screen dark for a while (a flash write the
// measured policy does not allow lit, retiring a scanned buffer, an update
// transfer, sleep, diagnostic panel restarts) holds a DarkLease instead of
// calling setBacklight(false) and hoping someone relights it later.
//
// States: Off (panel stopped), Dark (policy or lease says dark),
// Relighting (last lease released; waiting until the adapter reports a
// settled frame, i.e. the presented scene plus two VSYNCs), Lit.

#include <cstdint>

namespace lcd {

enum class DarkReason : uint8_t { Save, Retire, Update, Sleep, Diag, Count };

inline const char* darkReasonName(DarkReason reason) {
    switch (reason) {
    case DarkReason::Save: return "save";
    case DarkReason::Retire: return "retire";
    case DarkReason::Update: return "update";
    case DarkReason::Sleep: return "sleep";
    case DarkReason::Diag: return "diag";
    default: return "unknown";
    }
}

struct DisplayInputs {
    bool panelRunning = false;    // RGB panel started (panel.handle())
    bool screenOn = false;        // power session: not timed out / asleep
    bool chargeShowing = false;   // CHARGE ME screen is up
    bool quiet = false;           // manifest quiet hours apply now (trusted clock)
    bool installQueued = false;   // an install is queued: quiet hours never blank it
    uint32_t now = 0;             // millis()
    uint32_t touchWakeUntil = 0;  // a touch lit the screen during quiet hours
};

class DisplayOwner {
public:
    enum class State : uint8_t { Off, Dark, Relighting, Lit };
    struct Decision {
        bool lit;
        State state;
        const char* reason;
    };

    static constexpr uint32_t OVERDUE_MS = 10000;

    // The historical quiet rule (unchanged): quiet hours blank the screen
    // unless an install is queued or a touch woke it.
    bool quietBlanking(const DisplayInputs& in) const {
        if (!in.screenOn || in.chargeShowing) return false;
        return in.quiet && !in.installQueued && in.now >= in.touchWakeUntil;
    }

    // The historical backlightRefresh() policy, ignoring leases.
    bool policyLit(const DisplayInputs& in) const {
        if (!in.screenOn) return false;
        if (in.chargeShowing) return in.panelRunning;
        return !quietBlanking(in) && in.panelRunning;
    }

    // frameSettled: the adapter has seen the current scene presented plus
    // two VSYNCs since the last lease ended. Only consulted while relighting.
    Decision decide(const DisplayInputs& in, bool frameSettled) {
        if (!in.panelRunning) {
            relighting_ = false;
            return {false, State::Off, "panel-stopped"};
        }
        if (anyLease()) return {false, State::Dark, leaseReason()};
        if (!policyLit(in)) {
            relighting_ = false;
            if (!in.screenOn) return {false, State::Dark, "screen-off"};
            return {false, State::Dark, "quiet"};
        }
        if (relighting_) {
            if (!frameSettled) return {false, State::Relighting, "relight"};
            relighting_ = false;
        }
        return {true, State::Lit, "lit"};
    }

    void acquire(DarkReason reason, uint32_t now = 0) {
        auto& count = counts_[index(reason)];
        if (count == 0) since_[index(reason)] = now;
        if (count < UINT8_MAX) ++count;
    }

    void release(DarkReason reason) {
        auto& count = counts_[index(reason)];
        if (count == 0) return;
        if (--count == 0 && !anyLease()) relighting_ = true;
    }

    uint8_t held(DarkReason reason) const { return counts_[index(reason)]; }

    bool anyLease() const {
        for (auto count : counts_) if (count) return true;
        return false;
    }

    // Leases other than update and sleep should be short; the adapter logs
    // when one is held longer so a leaked lease is visible in captures.
    bool overdue(uint32_t now) const {
        for (unsigned i = 0; i < Reasons; ++i) {
            const auto reason = static_cast<DarkReason>(i);
            if (reason == DarkReason::Update || reason == DarkReason::Sleep) continue;
            if (counts_[i] && now - since_[i] >= OVERDUE_MS) return true;
        }
        return false;
    }

private:
    static constexpr unsigned Reasons = static_cast<unsigned>(DarkReason::Count);
    static unsigned index(DarkReason reason) {
        const unsigned i = static_cast<unsigned>(reason);
        return i < Reasons ? i : 0;
    }
    const char* leaseReason() const {
        static const char* const names[] = {"lease:save", "lease:retire", "lease:update",
                                            "lease:sleep", "lease:diag"};
        for (unsigned i = 0; i < Reasons; ++i) if (counts_[i]) return names[i];
        return "lease";
    }

    uint8_t counts_[Reasons]{};
    uint32_t since_[Reasons]{};
    bool relighting_ = false;
};

// Scoped lease: dark from construction to destruction on every exit path.
// needed=false makes it a no-op (e.g. the measured policy allows the write lit).
class DarkLease {
public:
    DarkLease(DisplayOwner& owner, DarkReason reason, bool needed = true, uint32_t now = 0)
        : owner_(needed ? &owner : nullptr), reason_(reason) {
        if (owner_) owner_->acquire(reason_, now);
    }
    ~DarkLease() { if (owner_) owner_->release(reason_); }
    DarkLease(const DarkLease&) = delete;
    DarkLease& operator=(const DarkLease&) = delete;

private:
    DisplayOwner* owner_;
    DarkReason reason_;
};

}  // namespace lcd
