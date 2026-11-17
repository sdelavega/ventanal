// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
#pragma once
#include <Preferences.h>
#include <cstring>
struct QuietHours {
    int start=-1, end=-1;
    bool valid() const { return start>=0 && start<1440 && end>=0 && end<1440; }
};
inline QuietHours quietHours;
inline int parseMinute(const char* s) {
    if(!s || strlen(s)!=5 || s[2]!=':') return -1;
    for(int i : {0,1,3,4}) if(s[i]<'0' || s[i]>'9') return -1;
    int h=(s[0]-'0')*10+s[1]-'0', m=(s[3]-'0')*10+s[4]-'0';
    return h<24 && m<60 ? h*60+m : -1;
}
inline void loadQuiet(Preferences& prefs) {
    quietHours={};
    QuietHours stored;
    if(prefs.getBytes("quiet", &stored, sizeof stored)==sizeof stored && stored.valid()) quietHours=stored;
}
inline bool storeQuiet(Preferences& prefs, QuietHours incoming) {
    if(!incoming.valid()) return false;
    if(prefs.putBytes("quiet", &incoming, sizeof incoming)!=sizeof incoming) return false;
    quietHours=incoming;
    return true;
}
inline int quietStart(int fallback) { return quietHours.valid()?quietHours.start:fallback; }
inline int quietEnd(int fallback) { return quietHours.valid()?quietHours.end:fallback; }
