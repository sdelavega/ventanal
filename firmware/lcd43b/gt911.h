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

    // Poll the first touch point. True while a finger is down, with its
    // position clamped to the 800x480 panel; the status buffer is always
    // acknowledged so stale points cannot accumulate.
    bool read(int& x, int& y) {
        uint8_t status = 0;
        if (!readRegister(0x814E, &status, 1)) return false;
        uint8_t clear = 0;
        writeRegister(0x814E, &clear, 1);
        if (!(status & 0x80) || !(status & 0x0F)) return false;
        uint8_t point[6] = {};
        if (!readRegister(0x814F, point, sizeof point)) return false;
        x = int(point[1]) | int(point[2]) << 8;
        y = int(point[3]) | int(point[4]) << 8;
        if (x < 0) x = 0;
        if (y < 0) y = 0;
        if (x > 799) x = 799;
        if (y > 479) y = 479;
        return true;
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