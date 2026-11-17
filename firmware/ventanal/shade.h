// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
#pragma once
#include "shade_tables.h"
#include <algorithm>

inline uint8_t shadeOpacity(int distance) {
    return shadeEdge[std::clamp(distance, -64, 64)+64];
}
// Floor integer square root; no runtime floating point or allocation.
inline int shadeRoot(unsigned value) {
    unsigned result=0, bit=1u<<30;
    while(bit>value) bit>>=2;
    while(bit) {
        if(value>=result+bit) { value-=result+bit; result=(result>>1)+bit; }
        else result>>=1;
        bit>>=2;
    }
    return static_cast<int>(result);
}
// Signed distance to (groupLeft-30, y+6, right, y+62), radius 30.
inline int shadeDistance(int x,int y,int groupLeft,int right,int top) {
    const int left=groupLeft-30;
    const int dx=std::max(left+30-x,x-(right-30));
    const int dy=std::max(top+6+30-y,y-(top+62-30));
    if(dx<=0 || dy<=0) return std::max(dx,dy)-30;
    return shadeRoot(static_cast<unsigned>(dx*dx+dy*dy))-30;
}
// The free rounded end spreads in two dimensions. A bounded distance
// correction matches the approved blurred mask there without changing its box.
// No correction at the panel-clipped right end or the flat upper/lower edges.
inline uint8_t shadeOpacityAt(int x,int y,int groupLeft,int right,int top) {
    const int correction=std::clamp((groupLeft-x)/2,0,6);
    return shadeOpacity(shadeDistance(x,y,groupLeft,right,top)+correction);
}
inline uint8_t shadeThreshold(int x,int y) {
#ifdef VENTANAL_SHADE_BAYER
    // 8x8 Bayer, scaled to bin centres in the same 0..255 threshold space.
    unsigned rank=0;
    for(unsigned bit=0;bit<3;++bit) {
        unsigned xb=(static_cast<unsigned>(x)>>bit)&1, yb=(static_cast<unsigned>(y)>>bit)&1;
        rank |= ((xb^yb)*2+yb)<<(4-2*bit);
    }
    return static_cast<uint8_t>(rank*4+2);
#else
    return shadeBlueNoise[(y&63)*64+(x&63)];
#endif
}
