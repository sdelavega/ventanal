// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
#pragma once
#include "../ventanal/owner.h"
#include <LittleFS.h>
#include <esp_heap_caps.h>
#include <algorithm>
#include <Preferences.h>
#include "frame.h"

namespace lcd {
inline const char* selectedArtId(const OwnerState& owner) {
    return owner.overrideArt.id[0] ? owner.overrideArt.id : owner.mainId;
}
// Copy before changing selection, exactly like e-paper. A later spare download
// must not change the owner's current override. Failed copies leave it intact.
inline bool copyArtOverride(const char* path, const char* id) {
    auto* buffer=static_cast<uint8_t*>(ps_malloc(32768));
    if(!buffer) return false;
    File source=LittleFS.open(path,"r"), out=LittleFS.open("/override.vnr.tmp","w");
    bool ok=bool(source) && bool(out) && source.size()==FRAME_BYTES;
    Sha256 hash;
    size_t copied=0;
    while(ok && copied<FRAME_BYTES) {
        const size_t n=std::min(size_t(32768),FRAME_BYTES-copied);
        ok=source.read(buffer,n)==n && out.write(buffer,n)==n;
        if(ok) { hash.update(buffer,n); copied+=n; }
    }
    source.close(); out.flush(); out.close(); free(buffer);
    char digest[17]; const auto* bytes=hash.digest();
    for(int i=0;i<8;++i) snprintf(digest+2*i,3,"%02x",bytes[i]);
    ok=ok && !strcmp(digest,id);
    ok=ok && LittleFS.rename("/override.vnr.tmp","/override.vnr");
    if(!ok) LittleFS.remove("/override.vnr.tmp");
    return ok;
}
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
    // Reject unterminated persisted ids before any shared owner operations.
    if(saved.mainId[16] || saved.overrideArt.id[16]) return false;
    for(const auto& spare:saved.spares) if(spare.id[16]) return false;
    for(const auto& pair:saved.queue.pairs) if(pair.skipped[16] || pair.shown[16]) return false;
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
