// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
#pragma once

#include <cstdint>
#include <vector>

using BYTE = uint8_t;
using UINT = unsigned;
struct JRECT { unsigned left, right, top, bottom; };
enum JRESULT { JDR_OK, JDR_INTR, JDR_INP, JDR_FMT1 };

struct JDEC {
    unsigned width = 0, height = 0;
    void* device = nullptr;
    std::vector<uint8_t> rgb;
};

JRESULT jd_prepare(JDEC*, UINT(*)(JDEC*, BYTE*, UINT), void*, UINT, void*);
JRESULT jd_decomp(JDEC*, UINT(*)(JDEC*, void*, JRECT*), BYTE);
