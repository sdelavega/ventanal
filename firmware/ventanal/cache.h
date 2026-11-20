// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
#pragma once
#include <Inkplate.h>
#include <array>
#include <cstdint>

struct ScreenInfo;
inline constexpr std::array<const char*, 8> frameCachePaths{
    "/art.vnf", "/weather.vnf", "/solar.vnf", "/extra0.vnf", "/extra1.vnf",
    "/spare0.vnf", "/spare1.vnf", "/spare2.vnf"
};
using ManifestFrameSlots = std::array<const ScreenInfo*, frameCachePaths.size()>;

bool fetchFrame(const char* url, const char* path);
bool canStageExtraFrame(const char* path);
// Call after fetching a valid manifest. Bit i marks a recovered cache slot.
uint8_t sweepFramePartials(const ManifestFrameSlots& frames);
bool blitFrame(const char* path, Inkplate& d);
