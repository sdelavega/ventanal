// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
#pragma once
#include "net.h"
#include <cstdio>
#include <cstring>

struct PressClassifier {
    int releasedAt=-1, pressedAt=-1;
    bool released=false, isDouble=false, isLong=false, continuous=true;
    // Only the initial uninterrupted LOW qualifies (inclusive 1500 ms).
    // Any HIGH, even a bounce, ends long eligibility. The original 600 ms
    // release/second-press window with 30 ms debounce remains unchanged.
    // A held second press is double only, never long; no release wait is
    // part of classification, and classification ends by 1500 ms.
    bool done(unsigned elapsed) const {
        return isDouble || isLong || (elapsed>600 && !continuous);
    }
    void sample(unsigned elapsed, bool pressed) {
        if(isDouble || isLong) return;
        if(!pressed) continuous=false;
        if(continuous && elapsed>=1500) { isLong=true; return; }
        if(elapsed>600) return;
        if(!released) {
            if(pressed) releasedAt=-1;
            else if(releasedAt<0) releasedAt=static_cast<int>(elapsed);
            else if(elapsed-static_cast<unsigned>(releasedAt)>=30) released=true;
        } else {
            if(!pressed) pressedAt=-1;
            else if(pressedAt<0) pressedAt=static_cast<int>(elapsed);
            else if(elapsed-static_cast<unsigned>(pressedAt)>=30) isDouble=true;
        }
    }
};
inline int nextSpare(int current, const bool cached[3]) {
    for(int i=current+1;i<3;i++) if(cached[i]) return i;
    return -1;
}
struct SkipPair { char skipped[17]{}, shown[17]{}; };
struct SkipQueue {
    SkipPair pairs[8]{};
    uint8_t count=0;
    void push(const char* skipped, const char* shown) {
        if(strlen(skipped)!=16 || strlen(shown)!=16 || !strcmp(skipped,shown)) return;
        if(count==8) { for(int i=1;i<8;i++) pairs[i-1]=pairs[i]; --count; }
        strcpy(pairs[count].skipped,skipped); strcpy(pairs[count].shown,shown); ++count;
    }
    void query(char* out, size_t size) const {
        if(!size) return;
        out[0]=0;
        size_t used=0;
        for(unsigned i=0;i<count;i++) {
            int n=snprintf(out+used,size-used,"%s%s~%s",i?",":"",pairs[i].skipped,pairs[i].shown);
            if(n<0 || static_cast<size_t>(n)>=size-used) return;
            used+=n;
        }
    }
    void acknowledge(bool success) { if(success) count=0; }
};
struct OwnerState {
    ScreenInfo spares[3]{};
    ScreenInfo overrideArt{};
    char mainId[17]{};
    int cursor=-1;
    SkipQueue queue{};
    void mainChanged(const char* id) {
        if(strcmp(mainId,id)) { strcpy(mainId,id); cursor=-1; overrideArt={}; }
    }
};
// Leave 75 s for local work, panel refresh, shutdown and watchdog margin.
inline uint32_t frameWakeRemainingMs(uint32_t elapsed) {
    constexpr uint32_t cutoffMs = 105000;
    return elapsed < cutoffMs ? cutoffMs - elapsed : 0;
}
