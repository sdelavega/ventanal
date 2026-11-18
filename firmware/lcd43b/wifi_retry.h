// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
#pragma once

#include <cstdint>

namespace lcd {
class WifiRetry {
public:
    void finished(uint32_t now, bool connected) {
        failedAt_ = now;
        wait_ = connected ? 0 : wait_ == 0 ? 15000 : wait_ < 240000 ? 2 * wait_ : 240000;
    }

    bool waiting() const { return wait_ != 0; }
    bool due(uint32_t now) const { return waiting() && now - failedAt_ >= wait_; }

private:
    uint32_t failedAt_ = 0;
    uint32_t wait_ = 0;
};
}  // namespace lcd
