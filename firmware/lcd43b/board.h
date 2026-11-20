// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
#pragma once

// Waveshare ESP32-S3-Touch-LCD-4.3B schematic, page 1:
// https://files.waveshare.com/wiki/ESP32-S3-Touch-LCD-4.3B/ESP32-S3-Touch-LCD-4.3B-Sch.pdf
// See README Hardware facts for USB-only measurements and limitations.
namespace lcd {
// VBAT feeds the charger/power path, not an ESP32 ADC or a divider.
constexpr bool BATTERY_ADC_AVAILABLE = false;
constexpr int BATTERY_ADC_GPIO = -1;
// Consumers must static_assert(BATTERY_ADC_AVAILABLE) before using this divider.
constexpr float BATTERY_DIVIDER = 0.0f;  // unavailable, not a conversion factor
// VBUS is not routed to a GPIO; CS8501 CHRG/DONE drive LEDs only.
constexpr bool POWER_SOURCE_DETECTABLE = false;
// DISP = CH422G EXIO2 -> AP3032 CTRL. No ESP32 LEDC connection.
constexpr bool BACKLIGHT_PWM = false;
constexpr int BACKLIGHT_EXIO = 2;
constexpr int BACKLIGHT_PWM_GPIO = -1;
constexpr int BACKLIGHT_PWM_CHANNEL = -1;
// U3 PCF85063ATL on SDA/SCL (GPIO8/9); INT pin 4 goes directly to GPIO6.
// CH422G OC1 is the DO1 isolated output via R25; IO6/IO7/OC2/OC3 are N.C.
constexpr bool RTC_PCF85063 = true;
constexpr int RTC_I2C_ADDRESS = 0x51;
constexpr int RTC_INT_GPIO = 6;
// TF slot in 1-bit mode. DAT3 / SPI CS is on the CH422G.
constexpr int SD_CLK_GPIO = 12;
constexpr int SD_CMD_GPIO = 11;
constexpr int SD_D0_GPIO = 13;
}  // namespace lcd
