// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
#pragma once

#include <cstdint>

namespace ventanal {

constexpr uint32_t kValidEpoch = 1704067200;
constexpr uint32_t kHour = 3600;
constexpr uint32_t kContentTimeout = 24 * kHour;

enum class RefreshLabel : uint8_t { None, Refreshing, UpToDate, Offline, Failed };

inline uint32_t trustedEpoch(int64_t now) {
    return now >= kValidEpoch && now <= UINT32_MAX ? uint32_t(now) : 0;
}

inline bool elapsed(uint32_t since, uint32_t now, uint32_t duration) {
    return since >= kValidEpoch && now >= since && now - since >= duration;
}

inline RefreshLabel labelFor(bool manualRefresh, bool anyFrameFailedPermanently,
                             uint32_t lastNewContentEpoch, uint32_t epochNow) {
    if (epochNow < kValidEpoch) return RefreshLabel::None;
    if (manualRefresh) return RefreshLabel::Refreshing;
    if (anyFrameFailedPermanently || elapsed(lastNewContentEpoch, epochNow, kContentTimeout))
        return RefreshLabel::Failed;
    return RefreshLabel::None;
}

inline int weatherAgeHours(uint32_t weatherEpoch, uint32_t epochNow) {
    return elapsed(weatherEpoch, epochNow, kHour) ? int((epochNow - weatherEpoch) / kHour) : -1;
}

// The RTC/RAM epoch is exact. After loss of retention, the latest checkpoint
// may be missing arrivals from its next 24 hours. Count from the end of
// that window (or now, if sooner) to avoid an early warning.
struct ContentStaleness {
    uint32_t lastNewEpoch;
    uint32_t checkpointEpoch;
    uint32_t checkpointAttempt;
    bool due;
    bool clockPendingContent;
    bool checkpointDirty;

    template<class Store>
    void restore(Store& prefs) {
        uint32_t stored[2]{};
        if (prefs.getBytes("content-epoch", stored, sizeof stored) == sizeof stored &&
            stored[0] >= kValidEpoch && stored[1] >= stored[0]) {
            checkpointEpoch = stored[0];
            checkpointAttempt = stored[1];
        }
    }

    void observe(uint32_t now, bool newContent = false) {
        clockPendingContent |= newContent;
        if (now < kValidEpoch) return;
        if (clockPendingContent) {
            lastNewEpoch = now;
            clockPendingContent = false;
            checkpointDirty = true;
        } else if (!lastNewEpoch) {
            lastNewEpoch = elapsed(checkpointAttempt, now, kContentTimeout)
                ? checkpointAttempt + kContentTimeout : now;
            checkpointDirty = !checkpointEpoch;
        }
    }

    bool checkpointDue(uint32_t now) const {
        return checkpointDirty && lastNewEpoch >= kValidEpoch && now >= lastNewEpoch &&
            (!checkpointAttempt || elapsed(checkpointAttempt, now, kContentTimeout));
    }

    template<class Store>
    void checkpoint(Store& prefs, uint32_t now) {
        checkpointAttempt = now;
        const uint32_t stored[] = {lastNewEpoch, now};
        if (prefs.putBytes("content-epoch", stored, sizeof stored) == sizeof stored) {
            checkpointEpoch = lastNewEpoch;
            checkpointDirty = false;
        }
    }

    RefreshLabel label(bool manual, uint32_t now) const {
        return labelFor(manual, false, due ? lastNewEpoch : 0, now);
    }
};

}  // namespace ventanal
