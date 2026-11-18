// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
#pragma once
#include "../ventanal/owner.h"
#include "frame.h"
#include <algorithm>
#include <Preferences.h>

namespace lcd {
inline const char* selectedArtId(const OwnerState& owner) {
    return owner.overrideArt.id[0] ? owner.overrideArt.id : owner.mainId;
}
// Pin the copied pixels before changing selection. A failed copy leaves
// the previous override and queue intact.
template<class Copy>
inline bool swapArt(OwnerState& owner, bool enabled, const bool cached[3], Copy copy) {
    if(!enabled) return false;
    const int next=nextSpare(owner.cursor,cached);
    if(next>=0 && !copy(next)) return false;
    char skipped[17]; strcpy(skipped,selectedArtId(owner));
    owner.cursor=next;
    owner.overrideArt=next<0 ? ScreenInfo{} : owner.spares[next];
    owner.queue.push(skipped,selectedArtId(owner));
    return true;
}
inline void saveOwner(Preferences& prefs, const OwnerState& owner) {
    prefs.putBytes("lcd-owner1",&owner,sizeof owner);
}
inline bool loadOwner(Preferences& prefs, OwnerState& owner) {
    OwnerState saved{};
    if(prefs.getBytesLength("lcd-owner1")!=sizeof saved ||
       prefs.getBytes("lcd-owner1",&saved,sizeof saved)!=sizeof saved ||
       saved.cursor < -1 || saved.cursor>2 || saved.queue.count>8) return false;
    // Skip query ids can be used literally because they are lowercase hex.
    const auto validId = [](const char (&id)[17]) {
        return !id[16] && (!id[0] || lowerHex(id, 16));
    };
    if (!validId(saved.mainId) || !validId(saved.overrideArt.id)) return false;
    for (const auto& spare : saved.spares) if (!validId(spare.id)) return false;
    for (unsigned i = 0; i < saved.queue.count; ++i) {
        const auto& pair = saved.queue.pairs[i];
        if (!pair.skipped[0] || !pair.shown[0] ||
            !validId(pair.skipped) || !validId(pair.shown)) return false;
    }
    owner=saved; return true;
}
// A successful manifest acknowledges only the snapshot actually sent. New taps
// may append (and overflow the eight-pair queue) while the worker is online.
inline void acknowledgeSkips(OwnerState& owner, uint32_t addedDuringFetch, bool success) {
    if(!success) return;
    const unsigned keep=std::min(unsigned(owner.queue.count),unsigned(addedDuringFetch));
    const unsigned drop=owner.queue.count-keep;
    for(unsigned i=0;i<keep;++i) owner.queue.pairs[i]=owner.queue.pairs[drop+i];
    owner.queue.count=uint8_t(keep);
}
} // namespace lcd
