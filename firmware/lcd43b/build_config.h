// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
#pragma once

#ifndef VENTANAL_FIRMWARE_VERSION
#define VENTANAL_FIRMWARE_VERSION "1.0.0"
#endif

namespace lcd {
inline constexpr char BOARD_ID[] = "lcd43b";
inline constexpr char FIRMWARE_VERSION[] = VENTANAL_FIRMWARE_VERSION;
inline constexpr char USER_AGENT[] = "Ventanal/" VENTANAL_FIRMWARE_VERSION;
}  // namespace lcd
