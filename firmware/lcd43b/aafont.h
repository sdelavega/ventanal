// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
#pragma once
#include <cstdint>
#include <cstddef>
namespace lcd {
struct AAGlyph {
    uint32_t bitmapOffset;
    uint8_t width, height, xAdvance;
    int8_t xOffset, yOffset;
};
struct AAFont {
    const uint8_t* bitmap;
    const AAGlyph* glyph;
    uint8_t first, last, yAdvance;
};
// Coverage is 0..15. Blend undimmed RGB565 channels with nearest rounding.
inline uint16_t blendAA(uint16_t bg,uint16_t fg,unsigned a) {
    const unsigned r=(((bg>>11)&31)*(15-a)+((fg>>11)&31)*a+7)/15;
    const unsigned g=(((bg>>5)&63)*(15-a)+((fg>>5)&63)*a+7)/15;
    const unsigned b=((bg&31)*(15-a)+(fg&31)*a+7)/15;
    return uint16_t((r<<11)|(g<<5)|b);
}
inline int advance(const AAFont& f,unsigned char c) {
    return c>=f.first && c<=f.last ? f.glyph[c-f.first].xAdvance : 0;
}
inline int textWidth(const AAFont& f,const char* s) {
    int width=0;
    for(;*s;++s) width+=advance(f,static_cast<unsigned char>(*s));
    return width;
}
inline void drawTextAA(uint16_t* px,int w,int h,int x,int baseline,
                       const char* s,const AAFont& f,uint16_t colour) {
    for(;*s;++s) {
        const auto c=static_cast<unsigned char>(*s);
        if(c<f.first || c>f.last) continue;
        const auto& g=f.glyph[c-f.first];
        for(int gy=0;gy<g.height;++gy) for(int gx=0;gx<g.width;++gx) {
            const int dx=x+g.xOffset+gx,dy=baseline+g.yOffset+gy;
            if(dx<0 || dy<0 || dx>=w || dy>=h) continue;
            const unsigned i=gy*g.width+gx;
            const unsigned a=(f.bitmap[g.bitmapOffset+i/2]>>(i%2?0:4))&15;
            if(a) px[size_t(dy)*w+dx]=blendAA(px[size_t(dy)*w+dx],colour,a);
        }
        x+=g.xAdvance;
    }
}
inline void drawText(uint16_t* px,int w,int h,const AAFont& f,const char* s,
                     int x,int baseline,uint16_t colour) {
    drawTextAA(px,w,h,x,baseline,s,f,colour);
}
} // namespace lcd
