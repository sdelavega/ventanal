// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
#include "esp_heap_caps.h"
#include "../lcd43b/owner.h"
#include <cassert>
#include <cstdio>
int main() {
    OwnerState owner;
    owner.mainChanged("aaaaaaaaaaaaaaaa");
    strcpy(owner.spares[0].id,"bbbbbbbbbbbbbbbb");
    strcpy(owner.spares[2].id,"cccccccccccccccc");
    bool cached[3]={true,false,true};
    auto copy=[](int){return true;};
    assert(!lcd::swapArt(owner,false,cached,copy) && owner.cursor==-1 && !owner.queue.count);
    assert(!lcd::swapArt(owner,true,cached,[](int){return false;}) && owner.cursor==-1);
    assert(lcd::swapArt(owner,true,cached,copy) && owner.cursor==0);
    assert(!strcmp(lcd::selectedArtId(owner),"bbbbbbbbbbbbbbbb"));
    assert(lcd::swapArt(owner,true,cached,copy) && owner.cursor==2);
    assert(lcd::swapArt(owner,true,cached,copy) && owner.cursor==-1);
    assert(!strcmp(lcd::selectedArtId(owner),owner.mainId) && owner.queue.count==3);
    char query[280]; owner.queue.query(query,sizeof query);
    assert(!strcmp(query,"aaaaaaaaaaaaaaaa~bbbbbbbbbbbbbbbb,bbbbbbbbbbbbbbbb~cccccccccccccccc,cccccccccccccccc~aaaaaaaaaaaaaaaa"));
    lcd::swapArt(owner,true,cached,copy);
    owner.mainChanged("aaaaaaaaaaaaaaaa"); assert(owner.cursor==0);
    Preferences prefs; assert(prefs.begin("ventanal",false));
    lcd::saveOwner(prefs,owner);
    OwnerState boot; assert(lcd::loadOwner(prefs,boot));
    assert(boot.cursor==0 && boot.queue.count==4 && !strcmp(boot.overrideArt.id,owner.overrideArt.id));
    lcd::acknowledgeSkips(boot,0,false); assert(boot.queue.count==4);
    lcd::swapArt(boot,true,cached,copy);
    lcd::acknowledgeSkips(boot,1,true);
    assert(boot.queue.count==1 && !strcmp(boot.queue.pairs[0].skipped,"bbbbbbbbbbbbbbbb"));
    for(int i=0;i<12;++i) lcd::swapArt(boot,true,cached,copy);
    lcd::acknowledgeSkips(boot,12,true); assert(boot.queue.count==8);
    lcd::acknowledgeSkips(boot,0,true); assert(!boot.queue.count);
    lcd::saveOwner(prefs,boot); OwnerState acked; assert(lcd::loadOwner(prefs,acked) && !acked.queue.count);
    boot.mainChanged("dddddddddddddddd"); assert(boot.cursor==-1 && !boot.overrideArt.id[0]);
    bool none[3]={}; assert(lcd::swapArt(boot,true,none,copy) && !boot.queue.count);
    // Production copy: failed IO/hash/rename never changes the committed override.
    files["/spare0.vnr"]=std::vector<uint8_t>(lcd::FRAME_BYTES,0);
    Sha256 hash; hash.update(files["/spare0.vnr"].data(),lcd::FRAME_BYTES);
    char id[17]; const auto* bytes=hash.digest();
    for(int i=0;i<8;++i) snprintf(id+2*i,3,"%02x",bytes[i]);
    assert(lcd::copyArtOverride("/spare0.vnr",id));
    auto saved=files["/override.vnr"];
    files["/spare0.vnr"][500]=1;
    assert(!lcd::copyArtOverride("/spare0.vnr",id) && files["/override.vnr"]==saved);
    files["/spare0.vnr"][500]=0; failWrite=true;
    assert(!lcd::copyArtOverride("/spare0.vnr",id)); failWrite=false; failRename=true;
    assert(!lcd::copyArtOverride("/spare0.vnr",id)); failRename=false;
    assert(files["/override.vnr"]==saved && !files.count("/override.vnr.tmp"));
    puts("lcd43b owner: shared spare cycle, copy failures, visibility, NVS, snapshot acknowledgement passed");
}
