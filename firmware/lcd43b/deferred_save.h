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

// Track the saved pair independently of frame availability and LittleFS.
class DeferredSelection {
public:
    static constexpr uint32_t IdleMs = 60000;
    static constexpr uint32_t RetryMs = 10000;

    void restored(int screen, int spare) {
        savedScreen_ = screen;
        savedSpare_ = spare;
        retrying_ = false;
    }

    void activity(uint32_t now) { lastActivity_ = now; }

    bool due(int screen, int spare, uint32_t now, bool backlightOn) const {
        return (screen != savedScreen_ || spare != savedSpare_) &&
            (!backlightOn || now - lastActivity_ >= IdleMs) &&
            (!retrying_ || now - failedAt_ >= RetryMs);
    }

    void failed(uint32_t now) {
        failedAt_ = now;
        retrying_ = true;
    }

private:
    int savedScreen_ = 0;
    int savedSpare_ = 0;
    uint32_t lastActivity_ = 0;
    uint32_t failedAt_ = 0;
    bool retrying_ = false;
};

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

    bool dueUnderPressure(uint32_t now) const {
        return body.bytes && ready(now);
    }

    bool replace(const char* nextId, const FrameBody& nextBody, uint32_t now) {
        size_t length = 0;
        while (length < sizeof id && nextId[length]) ++length;
        if (length == sizeof id) return false;

        if (!pending) queuedAt = now;  // updates cannot postpone the deadline
        memmove(id, nextId, length + 1);
        body = nextBody;
        pending = true;
        retrying = false;
        failedAt = 0;
        return true;
    }
};

}  // namespace lcd
