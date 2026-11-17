// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega

// The VENTANAL_LCD_DIAG_GESTURES sequence: fed through the production
// classifier at the loop's 10 ms poll cadence, it completes every gesture
// exactly once, in order, and leaves the classifier idle and reusable.

#include "../lcd43b/diag.h"

#include <cassert>
#include <cstdio>
#include <vector>

int main() {
    {   // The synthetic sequence completes next, double tap, previous and
        // refresh — once each, in order, at the poll cadence the loop uses.
        GestureClassifier classifier;
        std::vector<Gesture> fired;
        for (uint32_t t = 0; t < lcd::DIAG_DURATION_MS; t += 10) {
            bool touched = false;
            int x = 0, y = 0;
            assert(lcd::diagTouchSample(t, touched, x, y));
            const Gesture gesture = classifier.sample(t, touched, x, y);
            if (gesture != Gesture::None) fired.push_back(gesture);
        }
        assert(fired.size() == 4);
        assert(fired[0] == Gesture::Next);
        assert(fired[1] == Gesture::DoubleTap);
        assert(fired[2] == Gesture::Previous);
        assert(fired[3] == Gesture::Refresh);
        assert(!classifier.active());
    }
    {   // Every sample starts released; after the sequence the lookup ends
        // with a released finger and stops reporting.
        bool touched = true;
        int x = -1, y = -1;
        assert(lcd::diagTouchSample(0, touched, x, y));
        assert(touched && x == 700 && y == 240);
        assert(lcd::diagTouchSample(lcd::DIAG_LAST_STEP_MS, touched, x, y));
        assert(!touched);
        assert(!lcd::diagTouchSample(lcd::DIAG_DURATION_MS, touched, x, y));
        assert(!touched && x == 0 && y == 0);
        assert(!lcd::diagTouchSample(lcd::DIAG_DURATION_MS + 1000, touched, x, y));
    }
    {   // Handler-time tolerance: every fired gesture blocks the loop while
        // its handler runs (a blit ~700 ms, the refresh gesture seconds), so
        // sampling pauses and the synthetic clock jumps. The sequence must
        // still complete every gesture exactly once, in order.
        GestureClassifier classifier;
        std::vector<Gesture> fired;
        uint32_t t = 0;
        while (t < lcd::DIAG_DURATION_MS + 20000) {
            bool touched = false;
            int x = 0, y = 0;
            if (t < lcd::DIAG_DURATION_MS) lcd::diagTouchSample(t, touched, x, y);
            const Gesture gesture = classifier.sample(t, touched, x, y);
            if (gesture != Gesture::None) {
                fired.push_back(gesture);
                t += gesture == Gesture::Refresh ? 4000 : 700;   // handler time
                continue;
            }
            t += 10;
        }
        assert(fired.size() == 4);
        assert(fired[0] == Gesture::Next);
        assert(fired[1] == Gesture::DoubleTap);
        assert(fired[2] == Gesture::Previous);
        assert(fired[3] == Gesture::Refresh);
        assert(!classifier.active());
    }
    {   // The classifier stays reusable after the sequence: a later real
        // right-half tap, fed continuously at the loop cadence, still
        // classifies — and nothing extra fires around it.
        GestureClassifier classifier;
        for (uint32_t t = 0; t < lcd::DIAG_DURATION_MS; t += 10) {
            bool touched = false;
            int x = 0, y = 0;
            lcd::diagTouchSample(t, touched, x, y);
            classifier.sample(t, touched, x, y);
        }
        Gesture fired = Gesture::None;
        unsigned count = 0;
        for (uint32_t t = lcd::DIAG_DURATION_MS;
             t < lcd::DIAG_DURATION_MS + 3000; t += 10) {
            const uint32_t local = t - lcd::DIAG_DURATION_MS;
            const bool touched = local >= 1000 && local < 1400;  // one clean press
            const Gesture gesture = classifier.sample(t, touched, 700, 240);
            if (gesture != Gesture::None) {
                fired = gesture;
                ++count;
            }
        }
        assert(fired == Gesture::Next);
        assert(count == 1);
        assert(!classifier.active());
    }
    puts("lcd43b diag: sequence next, double, previous, refresh passed");
    return 0;
}
