// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
#pragma once

// Diagnostic gesture sequence for the VENTANAL_LCD_DIAG_GESTURES build flag.
// A table of synthetic touch states that, fed through the production
// GestureClassifier at the loop's 10 ms poll cadence, completes every
// gesture exactly once in order (next, double tap, previous, refresh) so the
// serial log can demonstrate the full gesture pipeline without a finger on
// the panel. Pure data + lookup, so the sequence itself is host-tested
// (test_lcd43b_diag); the sketch only includes this under the flag.
//
// Timing is handler-aware: every gesture handler blocks the loop (a blit
// takes ~600 ms, the refresh gesture seconds), so each gesture's edges come
// only after the previous gesture's handler has had time to finish — the
// double-tap's two presses straddle no handler at all. The refresh gesture
// goes last on purpose: its blocking HTTP refresh must not swallow any
// remaining edge (the release is cosmetic and supplied by the sequence end).

#include "gesture.h"

#include <cstdint>

namespace lcd {

struct DiagStep {
    uint32_t atMs;   // sequence time when this touch state begins
    bool touched;
    int x;
    int y;
};

constexpr DiagStep DIAG_SEQUENCE[] = {
    {0, true, 700, 240},      // right-half press -> Next
    {200, false, 700, 240},   // release; fires Next after the window
    {2000, true, 300, 200},   // double tap, first press
    {2150, false, 300, 200},
    {2350, true, 500, 300},   // second press; fires DoubleTap on release
    {2500, false, 500, 300},
    {4000, true, 100, 240},   // left-half press -> Previous
    {4200, false, 100, 240},  // fires Previous after the window
    {6000, true, 400, 240},   // long press -> Refresh
    {7800, false, 400, 240},  // release; only cleanup follows
};

constexpr uint32_t DIAG_LAST_STEP_MS = 7800;
constexpr uint32_t DIAG_DURATION_MS = 9000;

// Synthetic touch state at offsetMs into the sequence. Returns false once
// the sequence has finished (and reports a released finger).
inline bool diagTouchSample(uint32_t offsetMs, bool& touched, int& x, int& y) {
    if (offsetMs >= DIAG_DURATION_MS) {
        touched = false;
        x = 0;
        y = 0;
        return false;
    }
    const DiagStep* current = &DIAG_SEQUENCE[0];
    for (const DiagStep& step : DIAG_SEQUENCE)
        if (offsetMs >= step.atMs) current = &step;
    touched = current->touched;
    x = current->x;
    y = current->y;
    return true;
}

}  // namespace lcd
