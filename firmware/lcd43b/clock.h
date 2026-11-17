// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
#pragma once
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <initializer_list>
#include "board.h"
namespace lcd {
constexpr int MIN_VALID_YEAR = 2023;
inline bool validClock(const tm& t) {
    const int year=t.tm_year+1900;
    if(year<MIN_VALID_YEAR || year>2099 || t.tm_mon<0 || t.tm_mon>11
       || t.tm_wday<0 || t.tm_wday>6 || t.tm_hour<0 || t.tm_hour>23
       || t.tm_min<0 || t.tm_min>59 || t.tm_sec<0 || t.tm_sec>59) return false;
    const int days[]={31,28,31,30,31,30,31,31,30,31,30,31};
    const bool leap=year%4==0 && (year%100!=0 || year%400==0);
    return t.tm_mday>=1 && t.tm_mday<=days[t.tm_mon]+(t.tm_mon==1 && leap);
}
inline bool formatClock(const tm& t, bool h24, char* time, size_t tn, char* date, size_t dn) {
    if(tn) *time=0;
    if(dn) *date=0;
    if(!validClock(t)) return false;
    const char* days[]={"SUN","MON","TUE","WED","THU","FRI","SAT"};
    const char* months[]={"JAN","FEB","MAR","APR","MAY","JUN","JUL","AUG","SEP","OCT","NOV","DEC"};
    int n=h24 ? snprintf(time,tn,"%02d:%02d",t.tm_hour,t.tm_min)
              : snprintf(time,tn,"%d:%02d %s",t.tm_hour%12 ? t.tm_hour%12 : 12,t.tm_min,t.tm_hour<12?"AM":"PM");
    int d=snprintf(date,dn,"%s %d %s",days[t.tm_wday],t.tm_mday,months[t.tm_mon]);
    if(n>=0 && size_t(n)<tn && d>=0 && size_t(d)<dn) return true;
    if(tn) *time=0;
    if(dn) *date=0;
    return false;
}
inline int fromBcd(uint8_t b) { return (b&15)>9 || (b>>4)>9 ? -1 : (b>>4)*10+(b&15); }
inline uint8_t toBcd(int n) { return uint8_t((n/10)*16+n%10); }
// PCF85063 stores UTC, so timezone/DST changes cannot corrupt retained time.
// Bus access belongs exclusively to setup()/loop(), after GT911 bring-up.
template<class Bus> bool readRtc(Bus& bus, tm& t) {
    bus.beginTransmission(RTC_I2C_ADDRESS); bus.write(uint8_t(4));
    if(bus.endTransmission(false)!=0 || bus.requestFrom(uint8_t(RTC_I2C_ADDRESS),size_t(7))!=7) return false;
    uint8_t b[7]; for(auto& v:b) v=uint8_t(bus.read());
    if(b[0]&0x80) return false; // oscillator-stop flag: never trust this time
    tm candidate{};
    candidate.tm_sec=fromBcd(b[0]&0x7f); candidate.tm_min=fromBcd(b[1]&0x7f);
    candidate.tm_hour=fromBcd(b[2]&0x3f); candidate.tm_mday=fromBcd(b[3]&0x3f);
    candidate.tm_wday=b[4]&7; candidate.tm_mon=fromBcd(b[5]&0x1f)-1;
    const int year=fromBcd(b[6]); if(year<0) return false;
    candidate.tm_year=100+year;
    if(!validClock(candidate)) return false;
    t=candidate; return true;
}
template<class Bus> bool writeRtc(Bus& bus, const tm& utc) {
    if(!validClock(utc)) return false;
    bus.beginTransmission(RTC_I2C_ADDRESS); bus.write(uint8_t(4));
    // Writing seconds with bit 7 clear also clears OS after trusted NTP.
    for(int n : {utc.tm_sec,utc.tm_min,utc.tm_hour,utc.tm_mday,utc.tm_wday,utc.tm_mon+1,utc.tm_year-100}) bus.write(toBcd(n));
    return bus.endTransmission()==0;
}
inline time_t rtcEpoch(const tm& utc) {
    int days=0;
    for(int y=1970;y<utc.tm_year+1900;++y) days+=365+(y%4==0 && (y%100!=0 || y%400==0));
    const int lengths[]={31,28,31,30,31,30,31,31,30,31,30,31};
    for(int m=0;m<utc.tm_mon;++m) days+=lengths[m]+(m==1 && (utc.tm_year+1900)%4==0);
    days+=utc.tm_mday-1;
    return time_t(days)*86400+utc.tm_hour*3600+utc.tm_min*60+utc.tm_sec;
}
} // namespace lcd
