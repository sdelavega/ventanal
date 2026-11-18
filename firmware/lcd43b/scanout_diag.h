// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
#pragma once

#ifdef VENTANAL_LCD_DIAG_PANEL
#include <Arduino.h>
#include <atomic>
#include <esp_timer.h>
#include "log.h"
#endif

namespace lcd {

enum class ScanActivity : unsigned { Copy, Receive, Inflate, LittleFS, Nvs, Count };

#ifdef VENTANAL_LCD_DIAG_PANEL
inline std::atomic<uint32_t> scanActivity{0};
inline std::atomic<bool> scanLit{false};

class ScanScope {
public:
    explicit ScanScope(ScanActivity activity) : bit_(1u << unsigned(activity)),
        previous_(scanActivity.fetch_or(bit_, std::memory_order_relaxed)) {}
    ~ScanScope() {
        if (!(previous_ & bit_)) scanActivity.fetch_and(~bit_, std::memory_order_relaxed);
    }
private:
    uint32_t bit_;
    uint32_t previous_;
};

class ScanoutDiag {
public:
    void resetCadence() { previousStart_ = 0; }

    __attribute__((always_inline)) inline void fill(uint32_t start, int pos, int width, int height, int rows,
                        uint32_t drainUs, uint32_t blankUs, bool prefill) {
        const uint32_t elapsed = uint32_t(esp_timer_get_time()) - start;
        if (prefill) {
            resetCadence();
            return;
        }
        // EOF frees a buffer while the other drains. At wrap the interval
        // between EOFs also includes the 20 vertical blanking lines.
        const int next = previousPos_ + width * rows;
        const bool consecutive = pos == (next == width * height ? 0 : next);
        uint32_t budgetFromPrevious = drainUs * 2;
        if (pos == width * rows * 2) budgetFromPrevious += blankUs;
        const bool late = elapsed > drainUs ||
            (previousStart_ && consecutive && start - previousStart_ + elapsed > budgetFromPrevious);
        // VSYNC may re-prime the pair after a lost EOF. Retain that evidence
        // separately from individual EOF-delivered refill deadlines.
        if (previousStart_ && !consecutive) skipped_.fetch_add(1, std::memory_order_relaxed);
        previousStart_ = start;
        previousPos_ = pos;
        fills_.fetch_add(1, std::memory_order_relaxed);
        const auto activities = scanActivity.load(std::memory_order_relaxed);
        seen_.fetch_or(activities, std::memory_order_relaxed);
        const auto maximum = peak_.load(std::memory_order_relaxed);
        if (elapsed > maximum) peak_.store(elapsed, std::memory_order_relaxed);
        if (!late) return;
        (scanLit.load(std::memory_order_relaxed) ? litLate_ : darkLate_).fetch_add(1, std::memory_order_relaxed);
        if (!activities) idleLate_.fetch_add(1, std::memory_order_relaxed);
        for (unsigned i = 0; i < unsigned(ScanActivity::Count); ++i)
            if (activities & (1u << i)) lateByActivity_[i].fetch_add(1, std::memory_order_relaxed);
    }

    void report(uint32_t now, uint32_t drainUs) {
        const auto window = now - lastReport_;
        if (window < 1000) return;
        lastReport_ = now;
        const auto take = [](auto& counter) { return unsigned(counter.exchange(0, std::memory_order_relaxed)); };
        const auto fills = take(fills_);
        const auto peak = take(peak_);
        const auto lit = take(litLate_);
        const auto dark = take(darkLate_);
        const auto skipped = take(skipped_);
        const auto idle = take(idleLate_);
        const auto seen = take(seen_);
        unsigned causes[unsigned(ScanActivity::Count)];
        for (unsigned i = 0; i < unsigned(ScanActivity::Count); ++i) causes[i] = take(lateByActivity_[i]);
        log("bounce ms=%u span=%u fills=%u peak_us=%u budget_us=%u lit=%u dark=%u skips=%u active=%02x late_idle=%u copy=%u rx=%u inflate=%u fs=%u nvs=%u\n",
            unsigned(now), unsigned(window), fills, peak, unsigned(drainUs), lit, dark,
            skipped, seen, idle, causes[0], causes[1], causes[2], causes[3], causes[4]);
    }

private:
    uint32_t lastReport_ = 0;
    uint32_t previousStart_ = 0;
    int previousPos_ = 0;
    std::atomic<uint32_t> fills_{0}, peak_{0}, litLate_{0}, darkLate_{0}, skipped_{0}, idleLate_{0}, seen_{0};
    std::atomic<uint32_t> lateByActivity_[unsigned(ScanActivity::Count)]{};
};
#else
class ScanScope {
public:
    explicit ScanScope(ScanActivity) {}
};
#endif

} // namespace lcd
