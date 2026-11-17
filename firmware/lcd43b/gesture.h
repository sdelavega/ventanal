// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
#pragma once

// Touch gesture classification for the 800x480 LCD profile:
//   tap in the right half      -> next screen
//   tap in the left half       -> previous screen
//   double tap (second press)  -> next spare artwork, when spares publish
//   press held >= 1.5 s        -> refresh now
// Touch and release edges are debounced so capacitive bounce cannot create
// or erase a press. A completed long press ignores samples until release.

#include <cstdint>

enum class TapTarget { Next, Previous, OpenSettings };
inline TapTarget classifyTap(int x,int y,bool lcarsPage) {
    if(x>= (lcarsPage?540:550) && x<800 && y>=0 && y<(lcarsPage?64:80))
        return TapTarget::OpenSettings;
    return x>=400?TapTarget::Next:TapTarget::Previous;
}

enum class Gesture : uint8_t { None, Next, Previous, DoubleTap, Refresh };

class GestureClassifier {
public:
    static constexpr uint32_t DEBOUNCE_MS = 30;
    static constexpr uint32_t DOUBLE_WINDOW_MS = 400;
    static constexpr uint32_t LONG_PRESS_MS = 1500;
    static constexpr int PANEL_WIDTH = 800;
    static constexpr int HALF = PANEL_WIDTH / 2;

    // Feed the raw capacitive state (touched with its latest coordinates);
    // returns a completed gesture exactly once, or None while pending.
    Gesture sample(uint32_t nowMs, bool touched, int x, int y) {
        if (touched != raw_) {
            raw_ = touched;
            candidateSince_ = nowMs;
        }
        if (touched != settled_ && nowMs - candidateSince_ >= DEBOUNCE_MS) {
            settled_ = touched;
            const Gesture fired = settle(nowMs, touched, x, y);
            if (fired != Gesture::None) return fired;
        }
        return expire(nowMs);
    }

    int pressX() const { return pressX_; }
    int pressY() const { return pressY_; }
    void controlsMode(bool enabled) { *this=GestureClassifier{}; controls_=enabled; }

    // A press is active (possibly still unclassified).
    bool active() const {
        return state_ == State::Down || state_ == State::SecondDown;
    }

private:
    enum class State : uint8_t {
        Idle,          // no touch
        Down,          // first press held; may become a long press
        WaitSecond,   // released; waiting for the second press or timeout
        SecondDown,    // second press held: release is a double tap, but
                       // holding it past LONG_PRESS_MS refreshes first
        Locked,        // long press fired; ignore until release
    };

    Gesture settle(uint32_t nowMs, bool down, int x, int y) {
        switch (state_) {
        case State::Idle:
            if (down) {
                pressStart_ = nowMs;
                pressX_ = x;
                pressY_ = y;
                state_ = State::Down;
            }
            break;
        case State::Down:
            if (!down) {
                if(controls_) {
                    state_=State::Idle;
                    return pressX_>=HALF?Gesture::Next:Gesture::Previous;
                }
                releaseStart_ = nowMs;
                state_ = State::WaitSecond;
            }
            break;
        case State::WaitSecond:
            if (down) {
                secondStart_ = nowMs;
                state_ = State::SecondDown;
            }
            break;
        case State::SecondDown:
            if (!down) {
                state_ = State::Idle;
                return Gesture::DoubleTap;
            }
            break;
        case State::Locked:
            if (!down) state_ = State::Idle;
            break;
        }
        return Gesture::None;
    }

    Gesture expire(uint32_t nowMs) {
        // A long press refreshes from either press: holding the second
        // press past the window takes precedence over its double tap.
        if ((state_ == State::Down || state_ == State::SecondDown)
            && nowMs - (state_ == State::Down ? pressStart_ : secondStart_)
                   >= LONG_PRESS_MS) {
            state_ = State::Locked;
            return controls_?Gesture::None:Gesture::Refresh;
        }
        if (state_ == State::WaitSecond && nowMs - releaseStart_ > DOUBLE_WINDOW_MS) {
            state_ = State::Idle;
            return pressX_ >= HALF ? Gesture::Next : Gesture::Previous;
        }
        return Gesture::None;
    }

    State state_ = State::Idle;
    bool settled_ = false;
    bool raw_ = false;
    uint32_t candidateSince_ = 0;
    uint32_t pressStart_ = 0;
    uint32_t secondStart_ = 0;
    uint32_t releaseStart_ = 0;
    int pressX_ = 0, pressY_ = 0;
    bool controls_ = false;
};