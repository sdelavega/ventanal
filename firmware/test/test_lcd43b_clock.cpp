// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
#include "../lcd43b/clock.h"
#include <cassert>
#include <cstring>
#include <cstdio>
struct Bus {
    uint8_t regs[256] = {}, pos=0; bool selecting=true, fail=false;
    void beginTransmission(uint8_t address) { assert(address==0x51); selecting=true; }
    void write(uint8_t b) { if(selecting) {pos=b; selecting=false;} else regs[pos++]=b; }
    int endTransmission(bool = true) { return fail?2:0; }
    size_t requestFrom(uint8_t, size_t n) { return fail?0:n; }
    int read() { return regs[pos++]; }
};
int main() {
    tm t{}; t.tm_year=126; t.tm_mon=10; t.tm_mday=17; t.tm_wday=2; t.tm_hour=14; t.tm_min=32;
    char time[16], date[24];
    assert(lcd::formatClock(t,false,time,sizeof time,date,sizeof date));
    assert(!strcmp(time,"2:32 PM") && !strcmp(date,"TUE 17 NOV"));
    assert(lcd::formatClock(t,true,time,sizeof time,date,sizeof date) && !strcmp(time,"14:32"));
    for(int h : {0,12,23}) { t.tm_hour=h; assert(lcd::formatClock(t,false,time,sizeof time,date,sizeof date));
        assert(!strcmp(time,h==0?"12:32 AM":h==12?"12:32 PM":"11:32 PM")); }
    t.tm_year=70; assert(!lcd::formatClock(t,true,time,sizeof time,date,sizeof date));
    assert(!*time && !*date);
    t.tm_year=126;
    assert(!lcd::formatClock(t,true,time,2,date,sizeof date) && !*time && !*date);
    assert(!lcd::formatClock(t,true,time,sizeof time,date,2) && !*time && !*date);
    assert(!lcd::formatClock(t,true,nullptr,0,date,sizeof date) && !*date);
    Bus bus; t.tm_year=126; t.tm_hour=14; t.tm_sec=49;
    assert(lcd::rtcEpoch(t)==1794925969);
    assert(lcd::writeRtc(bus,t)); tm out{};
    assert(lcd::readRtc(bus,out)); assert(out.tm_year==126 && out.tm_hour==14 && out.tm_sec==49);
    bus.regs[4]|=0x80; assert(!lcd::readRtc(bus,out)); // OS forbids seeding
    assert(lcd::writeRtc(bus,t) && !(bus.regs[4]&0x80));
    bus.regs[5]=0x6a; assert(!lcd::readRtc(bus,out));
    lcd::writeRtc(bus,t); bus.regs[6]=0x24; assert(!lcd::readRtc(bus,out));
    lcd::writeRtc(bus,t); bus.regs[7]=0x31; assert(!lcd::readRtc(bus,out)); // November 31
    bus.fail=true; assert(!lcd::readRtc(bus,out) && !lcd::writeRtc(bus,t));
    puts("lcd43b clock: formatting, invalid time, RTC OS/calendar/I2C passed");
}
