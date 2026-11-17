// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
#pragma once

// The bridge between the GT911 driver and the gesture classifier. The
// sketch polls the controller every 10 ms, but a finger held on the panel
// only produces a fresh GT911 report every ~10-15 ms: polls between
// reports see NoUpdate and MUST keep the previous contact state, or every
// press looks like flicker and no gesture ever settles (the failing
// behaviour behind "real finger taps do nothing" — pinned by
// test_lcd43b_touch). Only a new report changes the state: >= 1 point is
// a contact (with its coordinates), zero points is a release, an I2C
// error keeps the previous state.

#include "gesture.h"
#include "gt911.h"

#include <cstdint>

namespace lcd {

// One poll's view of the controller, for diagnostics and tests: the raw
// report (plus the status register byte it came from) and the retained
// contact state after applying it.
struct TouchSample {
    Gt911::Report report = Gt911::Report::NoUpdate;
    uint8_t status = 0;
    bool contact = false;
    int x = 0;
    int y = 0;
};

class TouchPoller {
public:
    explicit TouchPoller(Gt911& device) : device_(&device) {}

    // Poll once and feed the production classifier; returns a completed
    // gesture exactly once, or None while pending. sample, when not null,
    // receives this poll's view of the controller.
    Gesture poll(uint32_t nowMs, TouchSample* sample = nullptr) {
        int x = 0, y = 0;
        uint8_t status = 0;
        const Gt911::Report report = device_->read(x, y, &status);
        switch (report) {
        case Gt911::Report::Contact:
            contact_ = true;
            x_ = x;
            y_ = y;
            break;
        case Gt911::Report::Release:
            contact_ = false;
            break;
        case Gt911::Report::NoUpdate:
        case Gt911::Report::Error:
            break;   // keep the previous contact state and coordinates
        }
        if (sample) *sample = {report, status, contact_, x_, y_};
        return classifier_.sample(nowMs, contact_, x_, y_);
    }

    // Synthetic sample path for the VENTANAL_LCD_DIAG_GESTURES replay: the
    // fake edge feeds the same classifier, and the retained state tracks
    // it so wake/logging behave exactly like real touch.
    Gesture sampleSynthetic(uint32_t nowMs, bool touched, int x, int y) {
        contact_ = touched;
        x_ = x;
        y_ = y;
        return classifier_.sample(nowMs, touched, x, y);
    }

    int pressX() const { return classifier_.pressX(); }
    int pressY() const { return classifier_.pressY(); }
    void controlsMode(bool enabled) { classifier_.controlsMode(enabled); }
    bool contact() const { return contact_; }
    int x() const { return x_; }
    int y() const { return y_; }
    bool active() const { return classifier_.active(); }

private:
    Gt911* device_;
    GestureClassifier classifier_;
    bool contact_ = false;
    int x_ = 0, y_ = 0;
};

}  // namespace lcd
