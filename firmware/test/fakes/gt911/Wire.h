// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
#pragma once

#include "Arduino.h"

#include <cstdint>
#include <cstring>
#include <deque>

// A fake GT911 controller for host tests of the production Gt911 driver:
// reports arrive on the fake clock only when their readyAtMs has passed,
// stay latched in 0x814E/0x814F until the host acknowledges 0x814E (the
// real controller holds the buffer-status bit until that clear write), and
// a zero-point report models a finger release. The driver never sees the
// queue — only the two registers, over this fake TwoWire.
struct FakeGt911State {
    struct Report {
        uint32_t readyAtMs;   // earliest fake-clock ms the report is visible
        uint8_t points;       // contact count; 0 = release
        uint16_t x;
        uint16_t y;
    };
    std::deque<Report> pending;
    size_t acks = 0;          // clear writes consumed by the host (0x814E=0)
    bool i2cFailure = false;  // every transaction NACKs while set

    void schedule(uint32_t readyAtMs, uint8_t points, uint16_t x, uint16_t y) {
        pending.push_back({readyAtMs, points, x, y});
    }

    // Register 0x814E as the controller would present it right now.
    uint8_t status() const {
        if (i2cFailure || pending.empty()) return 0;
        const Report& report = pending.front();
        return report.readyAtMs > fakeClock ? 0 : uint8_t(0x80u | report.points);
    }

    // Six point bytes at 0x814F: track id, x low, x high, y low, y high, size.
    void pointBytes(uint8_t out[6]) const {
        std::memset(out, 0, 6);
        if (i2cFailure || pending.empty()) return;
        const Report& report = pending.front();
        out[1] = uint8_t(report.x & 0xFF);
        out[2] = uint8_t(report.x >> 8);
        out[3] = uint8_t(report.y & 0xFF);
        out[4] = uint8_t(report.y >> 8);
    }

    void acknowledge() {
        if (i2cFailure || pending.empty()) return;
        pending.pop_front();
        ++acks;
    }
};

inline FakeGt911State gt911Device;
inline uint8_t gt911Address = 0x14;   // the address this board answers at

class TwoWire {
public:
    void begin(int, int) {}
    void setClock(uint32_t) {}

    void beginTransmission(uint8_t address) {
        address_ = address;
        writePos_ = 0;
    }

    size_t write(uint8_t value) {
        if (writePos_ < sizeof writeBuf_) writeBuf_[writePos_++] = value;
        return 1;
    }

    size_t write(const uint8_t* data, size_t length) {
        for (size_t i = 0; i < length; ++i) write(data[i]);
        return length;
    }

    // A repeated-start read (sendStop=false) just selects the register.
    uint8_t endTransmission(bool sendStop = true) {
        (void)sendStop;
        if (gt911Device.i2cFailure || address_ != gt911Address) return 2;
        if (writePos_ >= 2) {
            register_ = uint16_t(writeBuf_[0]) << 8 | writeBuf_[1];
            // The one acknowledged write the driver makes: clearing 0x814E
            // consumes the pending report, like the real controller.
            if (writePos_ == 3 && register_ == 0x814E && writeBuf_[2] == 0)
                gt911Device.acknowledge();
        }
        return 0;
    }

    uint8_t requestFrom(uint8_t address, size_t length) {
        if (gt911Device.i2cFailure || address != gt911Address) return 0;
        readRegister_ = register_;
        readLeft_ = length;
        return uint8_t(length);
    }

    int read() {
        if (!readLeft_) return -1;
        --readLeft_;
        const uint16_t reg = readRegister_++;
        if (reg == 0x814E) return gt911Device.status();
        if (reg >= 0x814F && reg <= 0x8154) {
            uint8_t point[6];
            gt911Device.pointBytes(point);
            return point[reg - 0x814F];
        }
        return 0;
    }

private:
    uint8_t address_ = 0;
    uint16_t register_ = 0;
    uint16_t readRegister_ = 0;
    uint8_t writeBuf_[8] = {};
    size_t writePos_ = 0;
    size_t readLeft_ = 0;
};

inline TwoWire Wire;
