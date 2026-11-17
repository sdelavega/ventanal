// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega

// GT911 driver + gesture-classifier integration. The controller reports a
// fresh point only every 10-15 ms while a finger is held, while the sketch
// polls at 10 ms: polls between reports must keep the previous contact
// state (NoUpdate), a valid zero-point report is a release, and a report
// must be read before its buffer status is acknowledged. Under those
// conditions every gesture still completes through the production driver
// and classifier.

#include "../lcd43b/gt911.h"
#include "../lcd43b/touch.h"

#include <cassert>
#include <cstdio>
#include <vector>

namespace {

// Models the GT911 address latch the datasheet (and Waveshare's/Espressif's
// 4.3B board support) describe: the controller samples its INT pin while
// RST rises — driven LOW selects 0x5D, driven HIGH or released to a
// pull-up selects 0x14 (the level this board's controller was observed
// answering at) — and the config version register only reads back nonzero
// once the controller booted its stored configuration. gt911BlankCycles
// lets a test emulate a controller that comes up blank for the first
// bring-up cycles (rapid-reset wedged state).
int gt911RisingEdges = 0;
int gt911BlankCycles = 0;
int firstRstLowMs = -1;
int intModeAtRise = -1;
int intLevelAtRise = -1;
int rstAssertedAtMs = -1;

bool setReset(bool asserted) {
    if (asserted) {
        if (rstAssertedAtMs < 0) rstAssertedAtMs = int(fakeClock);
    } else {
        ++gt911RisingEdges;
        intModeAtRise = fakePinMode(4);
        intLevelAtRise = fakePinLevel(4);
        gt911Address = intModeAtRise == OUTPUT && intLevelAtRise == LOW
                           ? Gt911::PRIMARY
                           : Gt911::SECONDARY;
        gt911ConfigVersion =
            gt911RisingEdges > gt911BlankCycles ? uint8_t(0x20) : uint8_t(0);
        if (firstRstLowMs < 0 && rstAssertedAtMs >= 0)
            firstRstLowMs = int(fakeClock) - rstAssertedAtMs;
    }
    return true;
}

void resetBringUpFake() {
    gt911RisingEdges = 0;
    gt911BlankCycles = 0;
    firstRstLowMs = -1;
    intModeAtRise = -1;
    intLevelAtRise = -1;
    rstAssertedAtMs = -1;
    gt911Address = 0x14;
    gt911ConfigVersion = 0x20;
    gt911Device.pending.clear();
    gt911Device.i2cFailure = false;
}

// Drives the poll loop exactly like the sketch: 10 ms cadence, one
// TouchPoller poll per tick, collecting completed gestures.
struct PollLoop {
    Gt911 device{Wire};            // production driver over the fake bus
    lcd::TouchPoller poller{device};
    uint32_t now = 0;
    std::vector<Gesture> fired;
    std::vector<lcd::TouchSample> samples;   // every poll's retained state

    PollLoop() {
        resetBringUpFake();
        device.begin(4, setReset);
        gt911Device.acks = 0;
    }

    void run(uint32_t milliseconds) {
        for (uint32_t i = 0; i < milliseconds / 10; ++i) {
            now += 10;
            fakeClock = now;
            lcd::TouchSample sample;
            const Gesture gesture = poller.poll(now, &sample);
            fired.push_back(gesture);
            samples.push_back(sample);
        }
    }

    std::vector<Gesture> gestures() const {
        std::vector<Gesture> out;
        for (Gesture gesture : fired)
            if (gesture != Gesture::None) out.push_back(gesture);
        return out;
    }

    // Polls whose retained state was "finger down" (for debounce checks).
    size_t downPolls() const {
        size_t count = 0;
        for (const auto& sample : samples)
            if (sample.contact) ++count;
        return count;
    }
};

// A held finger: fresh reports every 10-15 ms (press, moves, release).
void holdFinger(uint16_t x, uint16_t y, uint32_t forMs, uint32_t fromMs) {
    gt911Device.schedule(fromMs, 1, x, y);          // press
    for (uint32_t t = 10; t < forMs; t += 10 + (t / 3) % 6) {
        const uint32_t next = fromMs + t;
        if (next + 10 > fromMs + forMs) break;
        gt911Device.schedule(next, 1, uint16_t(x + (t / 100) % 7),
                             uint16_t(y + (t / 150) % 5));   // drifting hold
    }
    gt911Device.schedule(fromMs + forMs, 0, 0, 0);   // release report
}

}  // namespace

int main() {
    {   // The reference bring-up (Waveshare 4.3B board file + Espressif's
        // esp_lcd_touch_gt911) latches 0x5D: INT is driven LOW while RST
        // rises, TP_RST is held low for the full reference reset (100 ms,
        // the datasheet asks >= 10), and INT is released to a plain
        // floating input afterwards (the GT911 drives the line; a pull-up
        // would fight it).
        resetBringUpFake();
        Gt911 device{Wire};
        assert(device.begin(4, setReset));
        assert(device.address() == Gt911::PRIMARY);
        assert(device.cycles() == 1);
        assert(intModeAtRise == OUTPUT && intLevelAtRise == LOW);
        assert(firstRstLowMs >= 100);
        assert(fakePinMode(4) == INPUT);
    }
    {   // A controller that comes up with a blank configuration through
        // the reference cycle (config version 0, the observed wedged
        // state) is recovered by the power-cycle-equivalent long reset;
        // the accepted address is still the reference 0x5D.
        resetBringUpFake();
        gt911BlankCycles = 1;   // blank through the first bring-up cycle
        Gt911 device{Wire};
        assert(device.begin(4, setReset));
        assert(device.address() == Gt911::PRIMARY);
        assert(device.cycles() == 2);
        assert(intModeAtRise == OUTPUT && intLevelAtRise == LOW);
    }
    {   // A 1.8 s hold with 10-15 ms report spacing still refreshes: the
        // polls between reports keep the contact alive (the failing old
        // behaviour treated every empty buffer status as a release).
        PollLoop loop;
        holdFinger(400, 240, 1800, 0);
        loop.run(4000);
        const std::vector<Gesture> fired = loop.gestures();
        assert(fired.size() == 1);
        assert(fired[0] == Gesture::Refresh);
        assert(loop.downPolls() > 150);      // state persisted across gaps
        assert(!loop.poller.active());
        assert(gt911Device.acks > 100);     // every ready report acknowledged
    }
    {   // A quick tap on the right half is one Next gesture.
        PollLoop loop;
        gt911Device.schedule(0, 1, 700, 240);
        gt911Device.schedule(80, 0, 0, 0);
        loop.run(2000);
        const std::vector<Gesture> fired = loop.gestures();
        assert(fired.size() == 1);
        assert(fired[0] == Gesture::Next);
    }
    {   // A quick tap on the left half is one Previous gesture.
        PollLoop loop;
        gt911Device.schedule(0, 1, 100, 240);
        gt911Device.schedule(90, 0, 0, 0);
        loop.run(2000);
        const std::vector<Gesture> fired = loop.gestures();
        assert(fired.size() == 1);
        assert(fired[0] == Gesture::Previous);
    }
    {   // Two quick taps are one DoubleTap, even though each press only
        // reports twice before releasing.
        PollLoop loop;
        gt911Device.schedule(0, 1, 650, 100);
        gt911Device.schedule(12, 1, 650, 100);
        gt911Device.schedule(60, 0, 0, 0);
        gt911Device.schedule(250, 1, 500, 300);
        gt911Device.schedule(263, 1, 500, 300);
        gt911Device.schedule(320, 0, 0, 0);
        loop.run(3000);
        const std::vector<Gesture> fired = loop.gestures();
        assert(fired.size() == 1);
        assert(fired[0] == Gesture::DoubleTap);
    }
    {   // No report at all: NoUpdate polls never invent a touch.
        PollLoop loop;
        loop.run(3000);
        assert(loop.gestures().empty());
        assert(loop.downPolls() == 0);
        assert(gt911Device.acks == 0);
    }
    {   // I2C failures keep the previous state instead of reading as a
        // release: a press survives a transient bus error mid-hold.
        PollLoop loop;
        gt911Device.schedule(0, 1, 400, 240);
        loop.run(100);               // press seen and settled
        gt911Device.i2cFailure = true;
        loop.run(50);                // error polls: still down
        gt911Device.i2cFailure = false;
        gt911Device.schedule(200, 1, 400, 240);
        gt911Device.schedule(1900, 0, 0, 0);
        loop.run(4000);              // the hold continues into a long press
        const std::vector<Gesture> fired = loop.gestures();
        assert(fired.size() == 1);
        assert(fired[0] == Gesture::Refresh);
    }
    {
        PollLoop loop;
        const auto resets=gt911RisingEdges;
        loop.poller.suppressWakeTouch();
        holdFinger(650,20,1800,0); // wake cannot open Settings or refresh
        loop.run(2500);
        assert(loop.gestures().empty());
        assert(gt911RisingEdges==resets); // no address-select reset on wake
        gt911Device.schedule(2600,1,700,240);
        gt911Device.schedule(2700,0,0,0);
        loop.run(2000);
        assert(loop.gestures()==std::vector<Gesture>{Gesture::Next});
    }
    puts("lcd43b touch: 10-15ms GT911 cadence, no-update retention, release,"
         " bus errors passed");
    return 0;
}
