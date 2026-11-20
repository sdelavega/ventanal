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

// Manual refresh is immediate. Failure needs 24 hours overdue on a trusted clock.
inline RefreshLabel labelFor(bool manualRefresh, uint32_t overdueSince, uint32_t epochNow) {
    if (manualRefresh) return RefreshLabel::Refreshing;
    if (elapsed(overdueSince, epochNow, kContentTimeout))
        return RefreshLabel::Failed;
    return RefreshLabel::None;
}

inline int weatherAgeHours(uint32_t weatherEpoch, uint32_t epochNow) {
    return elapsed(weatherEpoch, epochNow, kHour) ? int((epochNow - weatherEpoch) / kHour) : -1;
}

struct ContentStaleness {
    uint32_t overdueSince;
    uint32_t checkpointEpoch;
    bool due;
    bool checkpointDirty;
    bool checkpointLoaded;
    bool legacyKeyRemoved;

    template<class Store>
    void restore(Store& prefs) {
        if (checkpointLoaded) return;
        checkpointLoaded = true;
        // The old content-epoch key recorded arrivals and cannot date an overdue period.
        uint32_t stored[2]{};
        if (prefs.getBytes("content-overdue", stored, sizeof stored) == sizeof stored &&
            stored[1] >= kValidEpoch &&
            (!stored[0] || (stored[0] >= kValidEpoch && stored[0] <= stored[1]))) {
            overdueSince = stored[0];
            checkpointEpoch = stored[1];
            due = overdueSince != 0;
        }
    }

    void clearOverdue() {
        if (overdueSince) {
            overdueSince = 0;
            checkpointDirty = true;
        }
    }

    void observe(uint32_t now, bool isDue) {
        due = isDue;
        if (!due) {
            clearOverdue();
        } else if (!overdueSince && now >= kValidEpoch) {
            overdueSince = now;
            checkpointDirty = true;
        }
    }

    bool checkpointDue(uint32_t now) const {
        return checkpointDirty && now >= kValidEpoch && now >= overdueSince &&
            (!checkpointEpoch || elapsed(checkpointEpoch, now, kContentTimeout));
    }

    template<class Store>
    void checkpoint(Store& prefs, uint32_t now) {
        const uint32_t stored[] = {overdueSince, now};
        if (prefs.putBytes("content-overdue", stored, sizeof stored) == sizeof stored) {
            if (!legacyKeyRemoved) {
                prefs.remove("content-epoch");
                legacyKeyRemoved = true;
            }
            checkpointEpoch = now;
            checkpointDirty = false;
        }
    }

    RefreshLabel label(bool manual, uint32_t now) const {
        return labelFor(manual, overdueSince, now);
    }
};

}  // namespace ventanal
