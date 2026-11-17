// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
#pragma once

// Minimal GT911 capacitive-touch reader for the 4.3B panel. The controller
// sits on the shared I2C bus with its interrupt on GPIO4 and reset driven
// through the CH422G expander (EXIO1). The reset sequence selects the bus
// address: INT released while RST rises gives 0x5D (the board's fitted
// default, as Waveshare's I2C scan shows), INT low gives 0x14; we probe both.

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

    // Reset the controller via the CH422G EXIO1 line and pick its address.
    // GT911 needs tens of milliseconds after reset before answering probes.
    bool begin(int irqPin, bool (*setReset)(bool)) {
        irqPin_ = irqPin;
        pinMode(irqPin, INPUT_PULLUP);
        delay(20);
        // INT released while RST rises selects 0x5D.
        if (resetAndProbe(setReset, true)) return finish();
        // INT driven low while RST rises selects 0x14.
        if (resetAndProbe(setReset, false)) return finish();
        // Give a slow controller one more chance to finish booting.
        delay(200);
        if (resetAndProbe(setReset, true)) return finish();
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

private:
    // released: the INT pin floats to its pull-up through the reset rise
    // (selecting 0x5D); otherwise it is driven low (selecting 0x14).
    bool resetAndProbe(bool (*setReset)(bool), bool released) {
        if (released) {
            pinMode(irqPin_, INPUT_PULLUP);
        } else {
            pinMode(irqPin_, OUTPUT);
            digitalWrite(irqPin_, LOW);
        }
        delay(20);
        setReset(true);     // assert reset: EXIO1 low
        delay(20);
        setReset(false);    // release; INT level while it rises picks the address
        delay(100);
        if (released) pinMode(irqPin_, INPUT_PULLUP);
        for (uint8_t candidate : {PRIMARY, SECONDARY}) {
            if (probe(candidate)) {
                address_ = candidate;
                return true;
            }
        }
        return false;
    }

    bool finish() {
        pinMode(irqPin_, INPUT_PULLUP);
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
};