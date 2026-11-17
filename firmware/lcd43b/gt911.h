// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
#pragma once

// Minimal GT911 capacitive-touch reader for the 4.3B panel. The controller
// sits on the shared I2C bus with its interrupt on GPIO4 and reset driven
// through the CH422G expander (EXIO1). The reset sequence selects the bus
// address: INT driven LOW while RST rises latches 0x5D — the address
// Waveshare's and Espressif's board support bring this panel up at — and
// INT HIGH (or released to its pull-up, the state at power-up) latches
// 0x14, the address this board's controller was observed answering at.

#include <Arduino.h>
#include <Wire.h>
#include <cstddef>
#include <cstdint>

class Gt911 {
public:
    static constexpr uint8_t PRIMARY = 0x5D;
    static constexpr uint8_t SECONDARY = 0x14;

    // One polled read of the controller. The sketch polls at 10 ms while
    // the controller reports a fresh point only every ~10-15 ms, so the
    // three outcomes must stay distinct (pinned by test_lcd43b_touch):
    //   NoUpdate - buffer-status bit clear: no new report; the caller
    //               must KEEP the previous contact state;
    //   Contact   - a ready report with >= 1 point (coords clamped to the
    //               800x480 panel); the report was read BEFORE 0x814E was
    //               acknowledged, so the controller cannot replace it
    //               mid-read;
    //   Release   - a ready report with zero points;
    //   Error      - I2C failure; keep the previous contact state.
    enum class Report : uint8_t { NoUpdate, Contact, Release, Error };

    explicit Gt911(TwoWire& bus) : bus_(&bus) {}

    // Bring the controller up with the reference sequence for this board
    // (ESP32_Display_Panel's 4.3B touch pre-begin plus esp_lcd_touch_
    // gt911): INT is driven LOW 10 ms before TP_RST is pulled low, stays
    // low while RST rises and for 200 ms afterwards, then is released to
    // a plain floating input (the GT911 drives INT from then on; a
    // pull-up would fight it). Longer/alternate sequences are tried only
    // while the previous one finds no controller with a non-blank
    // configuration (config version 0):
    //   1. reference sequence, TP_RST low 100 ms;
    //   2. power-cycle-equivalent sequence, TP_RST low 200 ms, in case
    //      rapid earlier resets left the controller wedged;
    //   3. INT-high sequence (latches 0x14), TP_RST low 200 ms.
    // GT911 needs tens of milliseconds after reset before answering
    // probes; every cycle waits >= 60 ms after INT is released before the
    // first register access.
    bool begin(int irqPin, bool (*setReset)(bool)) {
        irqPin_ = irqPin;
        cycles_ = 0;
        const struct { bool intHigh; uint16_t rstLowMs; } steps[] = {
            {false, 100}, {false, 200}, {true, 200},
        };
        for (size_t i = 0; i < sizeof steps / sizeof steps[0]; ++i) {
            ++cycles_;
            if (!resetAndProbe(setReset, steps[i].intHigh, steps[i].rstLowMs))
                continue;
            uint8_t version = 0;
            if (peek(0x8047, &version, 1) && version != 0) return finish();
            if (i + 1 == sizeof steps / sizeof steps[0])
                return finish();   // last resort: accept what answers
            // Blank configuration: retry with the next, longer reset.
        }
        pinMode(irqPin_, INPUT);   // nothing answered: leave INT floating
        return false;
    }

    Report read(int& x, int& y, uint8_t* rawStatus = nullptr) {
        uint8_t status = 0;
        if (!readRegister(0x814E, &status, 1)) return Report::Error;
        if (rawStatus) *rawStatus = status;
        if (!(status & 0x80)) return Report::NoUpdate;   // buffer not ready
        bool contact = false;
        int pointX = 0, pointY = 0;
        if (status & 0x0F) {
            uint8_t point[6] = {};   // track id, x lo/hi, y lo/hi, size lo
            if (!readRegister(0x814F, point, sizeof point)) return Report::Error;
            pointX = int(point[1]) | int(point[2]) << 8;
            pointY = int(point[3]) | int(point[4]) << 8;
            if (pointX < 0) pointX = 0;
            if (pointY < 0) pointY = 0;
            if (pointX > 799) pointX = 799;
            if (pointY > 479) pointY = 479;
            contact = true;
        }
        // Acknowledge only after the point data is consumed, so the
        // controller can never overwrite a report mid-read.
        uint8_t clear = 0;
        if (!writeRegister(0x814E, &clear, 1)) return Report::Error;
        if (!contact) return Report::Release;
        x = pointX;
        y = pointY;
        return Report::Contact;
    }

    uint8_t address() const { return address_; }

    // Reset cycles the last begin() used before a controller answered
    // (1 = the reference sequence), 0 when none did.
    uint8_t cycles() const { return cycles_; }

    // Raw 16-bit-register read for diagnostics/tests: the same protocol as
    // the report path (big-endian register address, repeated start,
    // requestFrom of exactly `length` bytes).
    bool peek(uint16_t reg, uint8_t* data, size_t length) {
        return readRegister(reg, data, length);
    }

private:
    // One reset/address-selection cycle. intHigh picks the level INT is
    // driven at while RST rises: LOW latches 0x5D (the reference), HIGH
    // latches 0x14. rstLowMs is how long TP_RST is held low.
    bool resetAndProbe(bool (*setReset)(bool), bool intHigh, uint16_t rstLowMs) {
        pinMode(irqPin_, OUTPUT);
        digitalWrite(irqPin_, intHigh ? HIGH : LOW);
        delay(10);              // INT settles before reset is asserted
        setReset(true);         // assert reset: EXIO1 low
        delay(rstLowMs);
        setReset(false);        // release; INT level while it rises latches
        delay(200);             // keep INT driven through the latch window
        pinMode(irqPin_, INPUT);   // release INT: plain floating input
        delay(60);              // >= 50 ms before the first register access
        for (uint8_t candidate : {PRIMARY, SECONDARY}) {
            if (probe(candidate)) {
                address_ = candidate;
                return true;
            }
        }
        return false;
    }

    bool finish() {
        uint8_t clear = 0;
        return writeRegister(0x814E, &clear, 1);
    }

    bool probe(uint8_t address) {
        bus_->beginTransmission(address);
        return bus_->endTransmission() == 0;
    }

    bool writeRegister(uint16_t reg, const uint8_t* data, size_t length) {
        bus_->beginTransmission(address_);
        bus_->write(uint8_t(reg >> 8));
        bus_->write(uint8_t(reg));
        bus_->write(data, length);
        return bus_->endTransmission() == 0;
    }

    bool readRegister(uint16_t reg, uint8_t* data, size_t length) {
        bus_->beginTransmission(address_);
        bus_->write(uint8_t(reg >> 8));
        bus_->write(uint8_t(reg));
        if (bus_->endTransmission(false) != 0) return false;
        if (bus_->requestFrom(address_, length) != length) return false;
        for (size_t i = 0; i < length; ++i) data[i] = bus_->read();
        return true;
    }

    TwoWire* bus_;
    int irqPin_ = 4;
    uint8_t address_ = PRIMARY;
    uint8_t cycles_ = 0;
};