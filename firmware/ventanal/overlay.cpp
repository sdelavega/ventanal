// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
#include "overlay.h"
#include "shade.h"
#include "fonts/Antonio-Bold.h"
#include "fonts/Antonio-Regular.h"
#include "fonts/Inter-Medium.h"
#include <algorithm>
#include <cctype>
#include <cstdio>

namespace {
// GFX fonts cover ASCII. A single 0x7f token represents a drawn middle dot,
// avoiding GFX's byte-wise UTF-8 handling. All painting is clipped to the manifest.
constexpr char dot = '\x7f';
int advance(const GFXfont& f, unsigned char c) {
    return c == 127 ? (f.glyph['H'-f.first].height==12 ? 6 : f.glyph['H'-f.first].height==15 ? 8 : 16) : (c >= f.first && c <= f.last ? f.glyph[c-f.first].xAdvance : 0);
}
int width(const GFXfont& f, const char* s) {
    int n=0; for (; *s; ++s) n+=advance(f, static_cast<unsigned char>(*s)); return n;
}
void pixel(Inkplate& d, const ReadoutRect& r, int x, int y, uint8_t color) {
    if(x>=r.x && y>=r.y && x<r.x+r.w && y<r.y+r.h) d.drawPixel(x,y,color);
}
void box(Inkplate& d, const ReadoutRect& r, int x, int y, int w, int h, uint8_t color, bool fill) {
    for(int j=0;j<h;++j) for(int i=0;i<w;++i)
        if(fill || i<2 || j<2 || i>=w-2 || j>=h-2) pixel(d,r,x+i,y+j,color);
}
// Draw every glyph at its natural width. Never rescale font pixels.
void text(Inkplate& d, const ReadoutRect& r, const GFXfont& f, const char* s,
          int left, int baseline, int available, uint8_t color, bool right=false) {
    const int natural=width(f,s); if(natural<=0 || available<=0) return;
    const int origin=right ? left+available-natural : left;
    int pen=0;
    for(;*s;++s) {
        const auto c=static_cast<unsigned char>(*s);
        if(c==127) {
            const int cap=f.glyph['H'-f.first].height;
            const int top=cap==12?-6:cap==15?-7:-15, left=cap==12?2:cap==15?3:6, size=cap<=15?2:4;
            for(int y=top;y<top+size;++y) for(int x=left;x<left+size;++x)
                pixel(d,r,origin+pen+x,baseline+y,color);
        } else if(c>=f.first && c<=f.last) {
            const GFXglyph& g=f.glyph[c-f.first];
            for(int y=0;y<g.height;++y) for(int x=0;x<g.width;++x) {
                const int bit=y*g.width+x;
                if(f.bitmap[g.bitmapOffset+bit/8] & (0x80>>(bit%8)))
                    pixel(d,r,origin+pen+g.xOffset+x,baseline+g.yOffset+y,color);
            }
        }
        pen+=advance(f,c);
    }
}
void battery(Inkplate& d,const ReadoutRect& r,int pct,bool low) {
    const int x=r.x+(r.lcars?26:22), y=r.y+(r.lcars?10:18);
    const int stride=r.lcars?22:8, w=r.lcars?16:6, h=r.lcars?26:16;
    const uint8_t color=low?INKPLATE_RED:INKPLATE_BLACK;
    const int filled=(std::clamp(pct,0,100)+24)/25;
    for(int i=0;i<4;++i) box(d,r,x+i*stride,y,w,h,color,i<filled);
    box(d,r,x+(r.lcars?88:32),y+(r.lcars?7:5),r.lcars?4:2,r.lcars?h-14:6,color,true);
}
}
void drawReadout(Inkplate& d,const ReadoutRect& r,int pct,bool offline,bool low,time_t drawnAt,bool clock24) {
    tm local{}; localtime_r(&drawnAt,&local);
    char clock[16],date[32],line[112];
    strftime(clock,sizeof clock,clock24?"%H:%M":"%I:%M %p",&local);
    if(r.lcars) battery(d,r,pct,low);
    if(r.lcars) {
        snprintf(line,sizeof line,"POWER %d%%",pct);
        text(d,r,Antonio_Bold18pt7b,line,r.x+124,r.y+36,r.w-144,INKPLATE_BLACK,true);
        snprintf(line,sizeof line,"LAST UPDATED %s",clock);
        text(d,r,Antonio_Regular18pt7b,line,r.x,r.y+88,r.w,INKPLATE_WHITE,true);
        strftime(date,sizeof date,"%a %d %b %Y",&local);
        for(char* p=date;*p;++p) *p=static_cast<char>(std::toupper(static_cast<unsigned char>(*p)));
        if(offline) {
            // Drop the year, then weekday, before drawing at natural width.
            const char* suffix=" \x7f OFFLINE";
            const int suffixWidth=width(Antonio_Regular18pt7b,suffix);
            for(const char* format : {"%a %d %b", "%d %b"}) {
                if(width(Antonio_Regular18pt7b,date)+suffixWidth<=r.w) break;
                strftime(date,sizeof date,format,&local);
                for(char* p=date;*p;++p) *p=static_cast<char>(std::toupper(static_cast<unsigned char>(*p)));
            }
            const int dateWidth=width(Antonio_Regular18pt7b,date);
            const int left=r.x+r.w-dateWidth-suffixWidth;
            text(d,r,Antonio_Regular18pt7b,date,left,r.y+124,dateWidth,INKPLATE_YELLOW);
            text(d,r,Antonio_Regular18pt7b,suffix,left+dateWidth,r.y+124,suffixWidth,INKPLATE_RED);
        } else text(d,r,Antonio_Regular18pt7b,date,r.x,r.y+124,r.w,INKPLATE_YELLOW,true);
    } else {
        const auto& f=Inter_Medium12cap7b;
        if(!clock24 && clock[0]=='0') {
            for(int i=0;clock[i];++i) clock[i]=clock[i+1];
        }
        const int available=std::max(1,r.w-28-25-8);
        // Drop weekday, then the whole date. Status and time stay legible.
        for(const char* format : {"%a %d %b", "%d %b", ""}) {
            if(*format) strftime(date,sizeof date,format,&local); else date[0]='\0';
            snprintf(line,sizeof line,"%d%% %c Updated %s%s%s%s",pct,dot,clock,
                     *date?" \x7f ":"",date,offline?" \x7f OFFLINE":"");
            if(width(f,line)<=available || !*date) break;
        }
        const int left=r.x+r.w-28-width(f,line);
        const int baseline=r.y+40; // production shade baseline y=40
        const int gx=left-8-25, gy=baseline+f.glyph['H'-f.first].yOffset;
        // Threshold only paints black; all other pixels retain the artwork.
        // Bound to the rect, including the Gaussian tail beyond the fitted box.
        for(int y=r.y;y<r.y+r.h;++y) for(int x=r.x;x<r.x+r.w;++x) {
            const auto opacity=shadeOpacityAt(x,y,gx,r.x+r.w,r.y);
            if(shadeThreshold(x,y)<opacity) pixel(d,r,x,y,INKPLATE_BLACK);
        }
        const uint8_t color=low?INKPLATE_RED:INKPLATE_WHITE;
        box(d,r,gx,gy,22,12,color,false);
        box(d,r,gx+22,gy+3,3,6,color,true);
        box(d,r,gx+4,gy+4,14*std::clamp(pct,0,100)/100,4,color,true);
        text(d,r,f,line,left,baseline,available,INKPLATE_WHITE);
    }
}
void drawWaiting(Inkplate& d,const char* url) {
    d.fillScreen(INKPLATE_WHITE);
    const ReadoutRect panel{0,0,1600,1200,false};
    text(d,panel,Inter_Medium15cap7b,"Waiting for host",120,560,1360,INKPLATE_BLACK);
    const auto& f=Inter_Medium15cap7b;
    int y=620;
    while(*url && y<1160) {
        char line[192]; size_t n=0; int used=0;
        while(url[n] && n<sizeof(line)-1 && used+advance(f,url[n])<=1360) {
            used+=advance(f,url[n]); line[n]=url[n]; ++n;
        }
        if(!n) break;
        line[n]='\0';
        text(d,panel,f,line,120,y,1360,INKPLATE_BLACK);
        url+=n; y+=32;
    }
}
void drawChargeMe(Inkplate& d) {
    d.fillScreen(INKPLATE_WHITE);
    const ReadoutRect panel{0,0,1600,1200,false};
    text(d,panel,Antonio_Bold18pt7b,"Charge me",120,600,1360,INKPLATE_RED);
}
