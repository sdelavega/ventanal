// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
#pragma once
#include <cstdint>

#ifdef VENTANAL_LCD_DIAG_PANEL
#include <Arduino.h>
#include <atomic>
#include <esp_timer.h>
#include "log.h"
#endif

namespace lcd {

struct ScanHealth {
    uint32_t lateRestarts = 0;
    uint32_t shortFrames = 0;
    uint32_t maxFillUs = 0;
    uint32_t maxVsyncGapUs = 0;
};

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
    void resetCadence() {
        previousStart_ = 0;
        lastActiveEof_.store(0, std::memory_order_relaxed);
        filling_.store(0, std::memory_order_relaxed);
        eofCount_.store(0, std::memory_order_relaxed);
        haveVsync_ = false;
    }

    __attribute__((always_inline)) inline void vsync(uint32_t now, uint32_t blankUs,
                                                     unsigned expectedEofs) {
        const unsigned completed = eofCount_.exchange(0, std::memory_order_relaxed);
        const unsigned lastEof = lastActiveEof_.exchange(0, std::memory_order_relaxed);
        if (haveVsync_) {
            if (filling_.load(std::memory_order_relaxed))
                overlappingFills_.fetch_add(1, std::memory_order_relaxed);
            if (completed < expectedEofs)
                shortFrames_.fetch_add(1, std::memory_order_relaxed);
            if (lastEof) {
                // IDF is about to reset GDMA. The last active EOF precedes
                // the vertical blank, within the fixed DMA/FIFO lead.
                const uint32_t phase = now - lastEof;
                if (phase < blankUs * 3) {
                    const auto largest = maxRestartPhase_.load(std::memory_order_relaxed);
                    if (phase > largest) maxRestartPhase_.store(phase, std::memory_order_relaxed);
                    if (phase > blankUs)
                        (scanLit.load(std::memory_order_relaxed) ? litRestarts_ : darkRestarts_)
                            .fetch_add(1, std::memory_order_relaxed);
                }
            }
        }
        haveVsync_ = true;
    }

    __attribute__((always_inline)) inline void beginFill(uint32_t start, bool lastActive) {
        filling_.fetch_add(1, std::memory_order_relaxed);
        if (lastActive) lastActiveEof_.store(start, std::memory_order_relaxed);
    }

    __attribute__((always_inline)) inline void dmaStatus(bool fifoUnderflow, bool descriptorError) {
        if (fifoUnderflow) fifoFlags_.fetch_add(1, std::memory_order_relaxed);
        if (descriptorError) descriptorFlags_.fetch_add(1, std::memory_order_relaxed);
    }

#ifdef VENTANAL_LCD_DIAG_PANEL_STALL
    __attribute__((always_inline)) inline bool inject(bool lastActive) {
        if (lastActive && scanLit.load(std::memory_order_relaxed) && ++litFrames_ == 64) {
            injected_.store(1, std::memory_order_relaxed);
            return true;
        }
        return false;
    }
#endif

    __attribute__((always_inline)) inline void fill(uint32_t start, int pos, int width, int height, int rows,
                        uint32_t drainUs, uint32_t blankUs, bool prefill) {
        const uint32_t elapsed = uint32_t(esp_timer_get_time()) - start;
        if (prefill) previousStart_ = 0;
        else eofCount_.fetch_add(1, std::memory_order_relaxed);
        filling_.fetch_sub(1, std::memory_order_relaxed);
        // EOF frees a buffer while the other drains. At wrap the interval
        // between EOFs also includes the vertical blanking lines.
        const int next = previousPos_ + width * rows;
        const bool consecutive = pos == (next == width * height ? 0 : next);
        uint32_t budgetFromPrevious = drainUs * 2;
        if (pos == width * rows * 2) budgetFromPrevious += blankUs;
        const bool late = elapsed > drainUs ||
            (previousStart_ && consecutive && start - previousStart_ + elapsed > budgetFromPrevious);
        // VSYNC may re-prime the pair after a lost EOF. Retain that evidence
        // separately from individual EOF-delivered refill deadlines.
        if (previousStart_ && !consecutive) skipped_.fetch_add(1, std::memory_order_relaxed);
        previousStart_ = prefill ? 0 : start;
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

    void report(uint32_t now, uint32_t drainUs, uint32_t blankUs) {
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
        log("bounce sync ms=%u short_frames=%u restart_lit=%u restart_dark=%u overlap=%u phase_us=%u vblank_us=%u fifo=%u descriptor=%u injected=%u\n",
            unsigned(now), take(shortFrames_), take(litRestarts_), take(darkRestarts_),
            take(overlappingFills_), take(maxRestartPhase_), unsigned(blankUs),
            take(fifoFlags_), take(descriptorFlags_), take(injected_));
    }

private:
    uint32_t lastReport_ = 0;
    uint32_t previousStart_ = 0;
    std::atomic<uint32_t> lastActiveEof_{0}, eofCount_{0}, filling_{0};
    bool haveVsync_ = false;
    int previousPos_ = 0;
    std::atomic<uint32_t> fifoFlags_{0}, descriptorFlags_{0};
    std::atomic<uint32_t> shortFrames_{0}, litRestarts_{0}, darkRestarts_{0}, injected_{0};
    std::atomic<uint32_t> maxRestartPhase_{0}, overlappingFills_{0};
#ifdef VENTANAL_LCD_DIAG_PANEL_STALL
    unsigned litFrames_ = 0;
#endif
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
