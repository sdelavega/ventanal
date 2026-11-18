// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
#pragma once

#include <cstdint>
#include <cstring>
#include <memory>

namespace lcd {

constexpr uint32_t MAX_SAVE_DELAY_MS = 6u * 60u * 60u * 1000u;

inline bool persistNow(bool backlightOn, bool quiet, uint32_t elapsedMs, bool pending) {
    // quiet means the quiet-hours blanking is active, excluding touch wake.
    return pending && (quiet || !backlightOn || elapsedMs >= MAX_SAVE_DELAY_MS);
}

struct FrameBody {
    std::shared_ptr<uint8_t> bytes;
    size_t size = 0;
};

struct DeferredSave {
    FrameBody body; // empty for a drop or an override sourced from disk
    char id[17]{};
    uint32_t queuedAt = 0;
    uint32_t failedAt = 0;
    bool pending = false;
    bool retrying = false;

    bool ready(uint32_t now) const {
        return pending && (!retrying || now - failedAt >= 10000);
    }

    void replace(const char* nextId, const FrameBody& nextBody, uint32_t now) {
        if (!pending) queuedAt = now;  // updates cannot postpone the deadline
        memcpy(id, nextId, strlen(nextId) + 1);
        body = nextBody;
        pending = true;
    }
};

}  // namespace lcd
