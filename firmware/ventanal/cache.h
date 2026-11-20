// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
#pragma once
#include <Inkplate.h>
bool fetchFrame(const char* url, const char* path);
bool canStageExtraFrame(const char* path);
void sweepFramePartials();
bool blitFrame(const char* path, Inkplate& d);
