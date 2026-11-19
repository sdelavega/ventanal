// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
//
// Diagnostic-build helpers: VSYNC cadence, touch bring-up dump, I2C scan.
//
// Part of lcd43b.ino's single translation unit: included only by lcd43b.ino,
// inside its anonymous namespace, in the order listed there. Earlier parts
// provide the declarations later parts use. Do not include it anywhere else.
#pragma once
#ifdef VENTANAL_LCD_DIAG_PANEL
// VSYNC cadence provides context. The bounce callback reports missed
// refill deadlines separately, even while VSYNC remains regular.
std::atomic<uint32_t> vsyncFrames{0};
uint32_t vsyncLastMs = 0; // ISR only
std::atomic<uint32_t> vsyncMaxGapMs{0};
uint32_t vsyncReportMs = 0;
uint32_t vsyncReportFrames = 0;
uint32_t stressStartMs = 0;
unsigned stressCycles = 0;
#ifdef VENTANAL_LCD_DIAG_F3
#ifdef VENTANAL_LCD_DIAG_DIM
unsigned dimStep = 0;
#endif
unsigned stressSettingsCycles = 0;
bool stressSettingsOpen = false;
bool scanOtherCore = false;
unsigned stressProfile = 0;
bool stressAdvance = false;
uint32_t writeProbeAt = 0;
unsigned writeProbeCount = 0;
#endif

bool IRAM_ATTR panelVsync(esp_lcd_panel_handle_t, const esp_lcd_rgb_panel_event_data_t*,
                void*) {
    const uint32_t now = millis();
    if (vsyncLastMs) {
        const uint32_t gap = now - vsyncLastMs;
        if (gap > vsyncMaxGapMs.load(std::memory_order_relaxed))
            vsyncMaxGapMs.store(gap, std::memory_order_relaxed);
    }
    vsyncLastMs = now;
    vsyncFrames.fetch_add(1, std::memory_order_relaxed);
    return false;
}

void panelVsyncReport(uint32_t now) {
    if (now - vsyncReportMs < 15000) return;
    const uint32_t frames = vsyncFrames.load(std::memory_order_relaxed);
    const uint32_t maxGap = vsyncMaxGapMs.load(std::memory_order_relaxed);
    const uint32_t window = vsyncReportMs ? (now - vsyncReportMs) / 1000 : 0;
    const uint32_t previous = vsyncReportFrames;
    vsyncReportMs = now;
    vsyncReportFrames = frames;
    lcd::log("panel vsync frames=%lu fps=%lu max_gap_ms=%lu\n",
             static_cast<unsigned long>(frames - previous),
             window ? static_cast<unsigned long>(
                          (frames - previous) / window)
                    : 0,
             static_cast<unsigned long>(maxGap));
}
#endif

#ifdef VENTANAL_LCD_DIAG_TOUCH
// Boot diagnostics for touch bring-up (diagnostic builds only): which bus
// addresses answer, the bring-up cycle the driver accepted, the GT911
// identity registers, the full configuration block 0x8047-0x80FF plus
// the fresh flag at 0x8100, the INT line level and the CH422G output
// shadow. The GT911 datasheet expects ASCII "911" at 0x8140 and a
// published configuration (nonzero version byte, X/Y resolution,
// touch number, module switch); an all-zero block means the controller
// came up blank or wedged.
void touchBootDiag() {
    Wire.beginTransmission(Gt911::PRIMARY);
    const bool primary = Wire.endTransmission() == 0;
    Wire.beginTransmission(Gt911::SECONDARY);
    const bool secondary = Wire.endTransmission() == 0;
    lcd::log("diag probe 0x%02x=%d 0x%02x=%d addr=0x%02x cycles=%d int=%d"
             " ch422g_io=0x%02x\n",
             Gt911::PRIMARY, primary, Gt911::SECONDARY, secondary,
             touch.address(), int(touch.cycles()), digitalRead(4),
             expander.outputs());
    uint8_t pid[3] = {}, fw[2] = {};
    const bool pidOk = touch.peek(0x8140, pid, sizeof pid);
    const bool fwOk = touch.peek(0x8144, fw, sizeof fw);
    lcd::log("diag gt911 product=%c%c%c ok=%d fw=0x%02x%02x ok=%d\n",
             pid[0], pid[1], pid[2], pidOk, fw[0], fw[1], fwOk);
    // Dump the configuration block in 16-byte reads: a single 185-byte
    // requestFrom exceeds the core's Wire receive buffer and fails.
    uint8_t cfg[185] = {};   // 0x8047..0x80FF, checksum last
    bool cfgOk = true;
    for (size_t off = 0; off < sizeof cfg; off += 16) {
        const size_t len = off + 16 <= sizeof cfg ? size_t(16)
                                                  : sizeof cfg - off;
        if (!touch.peek(uint16_t(0x8047 + off), cfg + off, len)) cfgOk = false;
        char line[88];
        int used = snprintf(line, sizeof line, "diag cfg %04x:",
                            0x8047 + int(off));
        if (used < 0) continue;
        for (size_t i = 0; i < len; ++i) {
            const int written =
                used < int(sizeof line)
                    ? snprintf(line + used, sizeof line - size_t(used),
                               " %02x", cfg[off + i])
                    : 0;
            if (written <= 0) break;
            used += written;
        }
        lcd::log("%s\n", line);
    }
    uint8_t fresh = 0;
    const bool freshOk = touch.peek(0x8100, &fresh, 1);
    // Config multi-byte fields are little-endian (unlike the big-endian
    // point coordinates): 800 -> 20 03, 480 -> e0 01.
    lcd::log("diag cfg ok=%d version=%d res=%dx%d touch_num=%d"
             " module_switch=0x%02x checksum=0x%02x fresh=%d fresh_ok=%d\n",
             cfgOk, cfg[0], int(cfg[2]) << 8 | cfg[1],
             int(cfg[4]) << 8 | cfg[3], cfg[5], cfg[6], cfg[184], fresh,
             freshOk);
    lcd::log("OWNER: tap now\n");
}
#endif

// Diagnostic only: report every I2C address that acknowledges a probe.
void scanBus() {
    char line[160] = "i2c scan:";
    size_t used = strlen(line);
    for (uint8_t address = 1; address < 0x78; ++address) {
        Wire.beginTransmission(address);
        if (Wire.endTransmission() == 0 && used < sizeof line - 8)
            used += snprintf(line + used, sizeof line - used, " %02x", address);
    }
    lcd::log("%s\n", line);
}
