// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
#include <cassert>
#include "../ventanal/shade.h"
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include "../ventanal/overlay.h"
#include "../ventanal/fonts/Antonio-Bold.h"
#include "../ventanal/fonts/Antonio-Regular.h"
#include "../ventanal/fonts/Inter-Medium.h"
int main(int argc, char** argv) {
    for(const auto* f : {&Antonio_Bold18pt7b, &Antonio_Regular18pt7b})
        assert(f->glyph['H'-f->first].height==30);
    assert(Inter_Medium12cap7b.glyph['H'-Inter_Medium12cap7b.first].height==12);
    assert(Inter_Medium15cap7b.glyph['H'-Inter_Medium15cap7b.first].height==15);
    setenv("TZ", "UTC0", 1); tzset();
    const time_t when = 1794753120; // 2026-11-15 14:32 UTC
    for (bool lcars : {true}) {
        ReadoutRect r{1236,30,334,136,true};
        for (int pct : {0, 1, 25, 26, 50, 74, 100}) {
            Inkplate d;
            drawReadout(d,r,pct,false,false,when,true);
            size_t ink=0;
            for (int y=0;y<1200;++y) for(int x=0;x<1600;++x) if(d.pixels[y*1600+x]!=255) {
                assert(x>=r.x && x<r.x+r.w && y>=r.y && y<r.y+r.h); ++ink;
            }
            assert(ink>500);
            // Each battery segment's centre is filled iff ceil(pct/25) reaches it.
            int gx=r.x+(lcars?26:22), gy=r.y+(lcars?10:18), stride=lcars?22:8;
            for(int i=0;i<4;++i) assert((d.pixels[(gy+(lcars?12:8))*1600+gx+i*stride+(lcars?6:3)]==INKPLATE_BLACK)==(i<(pct+24)/25));
            Inkplate low; drawReadout(low,r,pct,true,true,when,false);
            assert(low.pixels != d.pixels);
            assert(low.pixels[gy*1600+gx]==INKPLATE_RED);
        }
    }
    Inkplate a,b,c;
    ReadoutRect r{1236,30,334,136,true};
    drawReadout(a,r,75,false,false,when,true);
    drawReadout(b,r,75,false,false,when+60,true);
    drawReadout(c,r,75,false,false,when,false);
    assert(a.pixels!=b.pixels && a.pixels!=c.pixels);
    auto hash=[](const Inkplate& d) {
        uint64_t value=14695981039346656037ull;
        for(auto pixel:d.pixels) { value^=pixel; value*=1099511628211ull; }
        return value;
    };
    // Captured from the approved pre-fix renderer, both clock formats/statuses.
    assert(hash(a)==9306139020652798598ull);
    assert(hash(c)==7293979023496317238ull);
    Inkplate offline; drawReadout(offline,r,75,true,false,when,true);
    bool redStatus=false;
    for(int y=130;y<166;++y) for(int x=1236;x<1570;++x)
        redStatus |= offline.pixels[y*1600+x]==INKPLATE_RED;
    assert(redStatus);
    assert(hash(offline)==13151716567425455002ull);
    Inkplate offline12; drawReadout(offline12,r,75,true,false,when,false);
    assert(hash(offline12)==12705939001722044650ull);
    // All 84 real weekday/month combinations, at production width and a
    // narrower width that also exercises dropping the weekday. Render an
    // independent, unclipped reference row so clipping cannot hide lost ink.
    const auto& font=Antonio_Regular18pt7b;
    auto dateWidth=[&](const char* s) {
        int n=0;
        for(;*s;++s) n+=*s==127?16:font.glyph[*s-font.first].xAdvance;
        return n;
    };
    bool keptYear=false, droppedYear=false, droppedWeekday=false;
    for(int month=0;month<12;++month) for(int weekday=0;weekday<7;++weekday) {
        tm date{}; date.tm_year=126; date.tm_mon=month; date.tm_mday=10; date.tm_hour=14; date.tm_min=32;
        time_t stamp=mktime(&date);
        while(date.tm_wday!=weekday) { stamp+=86400; localtime_r(&stamp,&date); }
        for(int rectWidth : {334,260}) for(bool clock24 : {true,false}) {
            ReadoutRect rect{1236,30,static_cast<int16_t>(rectWidth),136,true};
            Inkplate actual; drawReadout(actual,rect,75,true,false,stamp,clock24);
            char label[32]; int choice=0;
            for(const char* format : {"%a %d %b %Y", "%a %d %b", "%d %b"}) {
                strftime(label,sizeof label,format,&date);
                for(char* p=label;*p;++p) *p=static_cast<char>(std::toupper(static_cast<unsigned char>(*p)));
                if(dateWidth(label)+dateWidth(" \x7f OFFLINE")<=rect.w) break;
                ++choice;
            }
            assert(choice<3);
            keptYear |= choice==0; droppedYear |= choice==1; droppedWeekday |= choice==2;
            Inkplate expected;
            int pen=rect.x+rect.w-dateWidth(label)-dateWidth(" \x7f OFFLINE");
            auto paint=[&](const char* s,uint8_t color) {
                for(;*s;++s) {
                    if(*s==127) {
                        for(int y=-15;y<-11;++y) for(int x=6;x<10;++x)
                            expected.drawPixel(pen+x,rect.y+124+y,color);
                        pen+=16;
                    } else {
                        const auto& g=font.glyph[*s-font.first];
                        for(int y=0;y<g.height;++y) for(int x=0;x<g.width;++x) {
                            const int bit=y*g.width+x;
                            if(font.bitmap[g.bitmapOffset+bit/8] & (0x80>>(bit%8))) {
                                const int px=pen+g.xOffset+x, py=rect.y+124+g.yOffset+y;
                                assert(px>=rect.x && px<rect.x+rect.w && py>=rect.y && py<rect.y+rect.h);
                                expected.drawPixel(px,py,color);
                            }
                        }
                        pen+=g.xAdvance;
                    }
                }
            };
            paint(label,INKPLATE_YELLOW);
            paint(" \x7f OFFLINE",INKPLATE_RED);
            // Exact natural-width date and complete OFFLINE glyphs, no missing columns.
            for(int y=rect.y+90;y<rect.y+rect.h;++y) for(int x=0;x<1600;++x)
                assert(actual.pixels[y*1600+x]==expected.pixels[y*1600+x]);
            for(int y=0;y<1200;++y) for(int x=0;x<1600;++x)
                if(actual.pixels[y*1600+x]!=255)
                    assert(x>=rect.x && x<rect.x+rect.w && y>=rect.y && y<rect.y+rect.h);
        }
    }
    assert(keptYear && droppedYear && droppedWeekday);
    const auto& artFont=Inter_Medium12cap7b;
    auto measure=[&](const char* line) {
        int n=0;
        for(;*line;++line) n+=*line==127?6:artFont.glyph[*line-32].xAdvance;
        return n;
    };
    const ReadoutRect shade{1020,0,580,104,false};
    // Independent natural-width reference for all field-drop choices, both
    // clocks, offline suffix, proportional fill and low-battery colour.
    for(bool clock24 : {true,false}) for(bool status : {true,false})
    for(int pct : {0,1,25,50,75,100}) for(bool low : {true,false}) {
        const std::string prefix=std::to_string(pct)+"% \x7f Updated "+(clock24?"14:32":"2:32 PM");
        const std::string suffix=status?" \x7f OFFLINE":"";
        const std::string full=prefix+" \x7f Sun 15 Nov"+suffix;
        const std::string noWeekday=prefix+" \x7f 15 Nov"+suffix;
        const std::string noDate=prefix+suffix;
        for(int rectWidth : {580,measure(full.c_str())+61,measure(noWeekday.c_str())+61,measure(noDate.c_str())+61,40}) {
            ReadoutRect rect{static_cast<int16_t>(1600-rectWidth),0,static_cast<int16_t>(rectWidth),104,false};
            const auto& expectedLine=measure(full.c_str())+61<=rect.w?full:
                measure(noWeekday.c_str())+61<=rect.w?noWeekday:noDate;
            const int textLeft=1572-measure(expectedLine.c_str());
            const int gx=textLeft-33, gy=29;
            Inkplate expected,actual;
            auto paint=[&](int x,int y,uint8_t color) {
                if(x>=rect.x && x<rect.x+rect.w && y>=rect.y && y<rect.y+rect.h)
                    expected.drawPixel(x,y,color);
            };
            const uint8_t color=low?INKPLATE_RED:INKPLATE_WHITE;
            for(int y=0;y<12;++y) for(int x=0;x<22;++x)
                if(x<2 || x>=20 || y<2 || y>=10) paint(gx+x,gy+y,color);
            for(int y=3;y<9;++y) for(int x=22;x<25;++x) paint(gx+x,gy+y,color);
            for(int y=4;y<8;++y) for(int x=0;x<14*pct/100;++x) paint(gx+4+x,gy+y,color);
            int pen=textLeft;
            for(unsigned char ch:expectedLine) {
                if(ch==127) {
                    for(int y=-6;y<-4;++y) for(int x=2;x<4;++x) paint(pen+x,40+y,INKPLATE_WHITE);
                    pen+=6;
                } else {
                    const auto& g=artFont.glyph[ch-artFont.first];
                    for(int y=0;y<g.height;++y) for(int x=0;x<g.width;++x) {
                        const int bit=y*g.width+x;
                        if(artFont.bitmap[g.bitmapOffset+bit/8] & (0x80>>(bit%8)))
                            paint(pen+g.xOffset+x,40+g.yOffset+y,INKPLATE_WHITE);
                    }
                    pen+=g.xAdvance;
                }
            }
            assert(pen==1572);
            drawReadout(actual,rect,pct,status,low,when,clock24);
            for(int y=0;y<1200;++y) for(int x=0;x<1600;++x) {
                auto want=expected.pixels[y*1600+x], got=actual.pixels[y*1600+x];
                if(want!=255) assert(got==want); // text/glyph overwrite shade
                else if(x<rect.x || x>=rect.x+rect.w || y<rect.y || y>=rect.y+rect.h) assert(got==255);
                else assert(got==255 || got==INKPLATE_BLACK);
            }
        }
    }
    int previous=228;
    for(int distance=-64;distance<=64;++distance) {
        int opacity=shadeOpacity(distance);
        assert(opacity<=previous); previous=opacity;
        const double expected=228*.5*std::erfc((distance-4)/(13*std::sqrt(2.0)));
        assert(std::abs(opacity-expected)<=.51);
    }
    assert(shadeOpacity(-28)==226);
    assert(shadeOpacity(0)==142 && shadeOpacity(14)==50);
    // Most core artwork must be black; round 0 retained too many bright dots.
    int coreBlack=0,corePixels=0;
    for(int y=24;y<44;++y) for(int x=1330;x<1530;++x) {
        coreBlack += shadeThreshold(x,y)<shadeOpacity(shadeDistance(x,y,1260,1600,0));
        ++corePixels;
    }
    assert(coreBlack>corePixels*.87 && coreBlack<corePixels*.91);
    // Two-dimensional blur loses more coverage at the rounded free end.
    // Keep nominal box/SDF fixed, but avoid a visibly heavy left feather.
    assert(shadeOpacityAt(1230,34,1260,1600,0)==100);
    assert(shadeOpacityAt(1248,34,1260,1600,0)==203);
    assert(shadeOpacityAt(1260,34,1260,1600,0)==226);
    assert(shadeOpacityAt(1400,34,1260,1600,0)==226);
    int histogram[256]{};
    for(auto threshold:shadeBlueNoise) ++histogram[threshold];
    for(int count:histogram) assert(count==16);
    assert(shadeDistance(1300,34,1200,1600,0)==-28);
    assert(shadeDistance(1170,34,1200,1600,0)==0);
    assert(shadeDistance(1300,6,1200,1600,0)==0);
    assert(shadeDistance(1600,34,1200,1600,0)==0);
    Inkplate retained,overlay;
    retained.fillScreen(4); // blue artwork, including outside the readout
    drawReadout(retained,shade,95,false,false,when,false);
    drawReadout(overlay,shade,95,false,false,when,false);
    for(size_t i=0;i<retained.pixels.size();++i)
        assert(retained.pixels[i]==(overlay.pixels[i]==255?4:overlay.pixels[i]));
    Inkplate shortLine,longLine;
    drawReadout(shortLine,shade,1,false,false,when,true);
    drawReadout(longLine,shade,100,true,false,when,false);
    auto leftEdge=[](const Inkplate& d) {
        for(int x=0;x<1600;++x) for(int y=0;y<104;++y)
            if(d.pixels[y*1600+x]==INKPLATE_BLACK) return x;
        return 1600;
    };
    assert(leftEdge(longLine)<leftEdge(shortLine));
    assert(leftEdge(longLine)>=shade.x);
    bool top=false,right=false;
    for(int x=shade.x;x<1600;++x) top |= shortLine.pixels[x]==INKPLATE_BLACK;
    for(int y=0;y<104;++y) right |= shortLine.pixels[y*1600+1599]==INKPLATE_BLACK;
#ifndef VENTANAL_SHADE_BAYER
    assert(top && right);
#else
    (void)top; (void)right; // Compare Bayer's phase artifacts in exported crops.
#endif
    // Long URLs wrap onto subsequent lines.
    Inkplate longUrl;
    std::string url="http://host:8765"+std::string(250,'W');
    drawWaiting(longUrl,url.c_str());
    bool wrapped=false;
    for(int y=635;y<700;++y) for(int x=120;x<1480;++x)
        wrapped |= longUrl.pixels[y*1600+x]==INKPLATE_BLACK;
    assert(wrapped);
    if(argc>1) {
        time_t sampleTime=1794838440; // Mon 16 Nov 2026 14:14 UTC
        int samplePct=95;
        if(argc>2) {
            int widest=0;
            for(int day=0;day<365;++day) for(int minute=0;minute<1440;++minute) {
                const time_t stamp=1767225600+day*86400+minute*60;
                tm local{}; localtime_r(&stamp,&local);
                char clock[16],date[32],label[112];
                strftime(clock,sizeof clock,"%I:%M %p",&local);
                strftime(date,sizeof date,"%a %d %b",&local);
                snprintf(label,sizeof label,"100%% %c Updated %s %c %s %c OFFLINE",127,
                         clock+(clock[0]=='0'),127,date,127);
                if(measure(label)>widest) { widest=measure(label); sampleTime=stamp; }
            }
            samplePct=100;
            tm local{}; localtime_r(&sampleTime,&local);
            char date[80]; strftime(date,sizeof date,"%a %d %b %I:%M %p",&local);
            printf("widest offline: 100%%, %s; text width=%d, group width=%d\n",date,widest,widest+33);
            assert(widest+61<=shade.w);
        }
        Inkplate sample; drawReadout(sample,shade,samplePct,argc>2,false,sampleTime,false);
        FILE* out=fopen(argv[1], "wb"); assert(out);
        // PGM preserves untouched=255, white=1 and red=3 for compositing.
        fprintf(out,"P5\n1600 1200\n255\n");
        fwrite(sample.pixels.data(),1,sample.pixels.size(),out);
        fclose(out);
    }
    Inkplate waiting; drawWaiting(waiting,"http://host:8765");
    Inkplate charge; drawChargeMe(charge);
    assert(waiting.pixels!=charge.pixels);
    puts("overlay shade geometry, natural widths, dropping, clipping, battery, weather and fallback screens passed");
}
