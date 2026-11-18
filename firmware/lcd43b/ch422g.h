// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
#pragma once

// CH422G I/O expander on the Waveshare ESP32-S3-Touch-LCD-4.3B. The chip
// shares the board's I2C bus (SDA=GPIO8, SCL=GPIO9); registers are selected
// by I2C device address rather than a register pointer. Power-on defaults
// already release resets and enable the backlight; we keep a shadow of the
// IO0-7 output byte so unrelated lines are never disturbed.

#include <Wire.h>
#include <cstdint>

class Ch422g {
public:
    // 7-bit I2C device addresses select the CH422G registers.
    static constexpr uint8_t ADDR_WR_SET = 0x24;  // bit0: IO0-7 output enable
    static constexpr uint8_t ADDR_WR_OC = 0x23;   // OC0-3 open-drain levels
    static constexpr uint8_t ADDR_WR_IO = 0x38;   // IO0-7 output levels
    static constexpr uint8_t ADDR_RD_IO = 0x26;   // IO0-7 inputs
    // Board wiring: EXIO1 = GT911 reset, EXIO2 = backlight enable,
    // EXIO3 = LCD reset, EXIO4 = TF card select (stays deselected).
    static constexpr uint8_t TP_RST = 1u << 1;
    static constexpr uint8_t DISP = 1u << 2;
    static constexpr uint8_t LCD_RST = 1u << 3;
    static constexpr uint8_t SD_CS = 1u << 4;

    explicit Ch422g(TwoWire& bus) : bus_(&bus) {}

    // IO_OE is shared. Leave DI0/DI1 high while unused, release reset,
    // deselect the card and start with DISP low.
    bool begin() {
        if (!write(ADDR_WR_SET, 0x01)) return false;
        outputs_ = uint8_t(0xFF & ~DISP);
        return write(ADDR_WR_IO, outputs_);
    }

    bool setBacklight(bool on) {
        if (!setBit(DISP, on) || bus_->requestFrom(ADDR_RD_IO, uint8_t(1)) != 1) return false;
        const int readback = bus_->read();
        return readback >= 0 && bool(readback & DISP) == on;
    }

    // asserted drives the reset line low.
    bool setTouchReset(bool asserted) { return setBit(TP_RST, !asserted); }
    bool setLcdReset(bool asserted) { return setBit(LCD_RST, !asserted); }

    // Current backlight state as last written.
    bool backlightOn() const { return outputs_ & DISP; }

    // Shadow of the IO0-7 output byte as last written (diagnostics).
    uint8_t outputs() const { return outputs_; }

private:
    bool setBit(uint8_t bit, bool high) {
        const uint8_t next = high ? uint8_t(outputs_ | bit) : uint8_t(outputs_ & ~bit);
        if (!write(ADDR_WR_IO, next)) return false;
        outputs_ = next;
        return true;
    }

    bool write(uint8_t address, uint8_t value) {
        bus_->beginTransmission(address);
        bus_->write(value);
        return bus_->endTransmission() == 0;
    }

    TwoWire* bus_;
    uint8_t outputs_ = uint8_t(0xFF & ~DISP);
};