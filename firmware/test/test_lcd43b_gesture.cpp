// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega

// Touch gesture classification: right half taps next, left half taps back,
// a second press doubles into a spare cycle, 1.5 s holds refresh, and all
// edges are debounced against capacitive bounce.

#include "../lcd43b/gesture.h"
#include <cassert>
#include <cstdio>

namespace {

struct Feeder {
    GestureClassifier classifier;
    uint32_t t = 0;
    Gesture tick(bool touched, int x, int y, uint32_t step = 40) {
        t += step;   // every step exceeds the 30 ms debounce window
        return classifier.sample(t, touched, x, y);
    }

    bool active() const { return classifier.active(); }
};

Gesture completeTap(Feeder& f, int x, int y) {
    f.tick(true, x, y);                       // raw press edge
    assert(f.tick(true, x, y) == Gesture::None);   // press settles
    f.tick(false, x, y);                      // raw release edge
    assert(f.tick(false, x, y) == Gesture::None); // release settles
    while (true) {
        const Gesture fired = f.tick(false, x, y, 100);
        if (fired != Gesture::None) return fired;
    }
}

}  // namespace

int main() {
    {   // Tap right half advances; left half goes back; the middle is right.
        Feeder right;
        assert(completeTap(right, 700, 240) == Gesture::Next);
        assert(completeTap(right, 799, 479) == Gesture::Next);
        assert(completeTap(right, 400, 10) == Gesture::Next);
        Feeder left;
        assert(completeTap(left, 399, 240) == Gesture::Previous);
        assert(completeTap(left, 0, 0) == Gesture::Previous);
    }
    {   // Two presses inside the window are one double tap.
        Feeder f;
        f.tick(true, 650, 100);
        f.tick(true, 650, 100);
        f.tick(false, 650, 100);
        assert(f.tick(false, 650, 100) == Gesture::None);   // waiting window
        f.tick(true, 300, 200);
        assert(f.tick(true, 300, 200) == Gesture::None);    // second press settles
        assert(f.tick(false, 300, 200) == Gesture::None);   // raw release
        assert(f.tick(false, 300, 200) == Gesture::DoubleTap);
    }
    {   // A hold of at least 1.5 s refreshes; nothing fires before it.
        Feeder f;
        f.tick(true, 400, 240);
        assert(f.tick(true, 400, 240) == Gesture::None);   // press settles
        assert(f.tick(true, 400, 240, 1400) == Gesture::None);
        assert(f.tick(true, 400, 240, 120) == Gesture::Refresh);
        assert(f.tick(true, 400, 240) == Gesture::None);   // stays silent
        assert(f.tick(false, 400, 240) == Gesture::None);  // raw release
        assert(f.tick(false, 400, 240) == Gesture::None);  // release settles
        assert(completeTap(f, 100, 100) == Gesture::Previous);   // reusable
    }
    {   // A second press held beyond the long-press window refreshes too:
        // long press takes precedence over the double tap.
        Feeder f;
        f.tick(true, 500, 240);
        f.tick(true, 500, 240);
        f.tick(false, 500, 240);
        f.tick(false, 500, 240);
        f.tick(true, 500, 240);
        assert(f.tick(true, 500, 240) == Gesture::None);
        assert(f.tick(true, 500, 240, 2000) == Gesture::Refresh);
        assert(f.tick(false, 500, 240) == Gesture::None);  // raw release
        assert(f.tick(false, 500, 240) == Gesture::None);  // unlocks
        assert(completeTap(f, 700, 100) == Gesture::Next);  // reusable
    }
    {   // Bounce pulses shorter than the debounce window never press.
        Feeder f;
        for (int i = 0; i < 10; ++i) {
            assert(f.tick(true, 700, 240, 10) == Gesture::None);
            assert(f.tick(false, 700, 240, 10) == Gesture::None);
        }
        assert(completeTap(f, 700, 240) == Gesture::Next);
    }
    {   // A press active while unclassified is reported.
        Feeder f;
        f.tick(true, 700, 240);
        f.tick(true, 700, 240);
        assert(f.active());
        f.tick(false, 700, 240);
        f.tick(false, 700, 240);
        assert(!f.active());
    }
    puts("lcd43b gesture: halves, double tap, long press, debounce passed");
    return 0;
}