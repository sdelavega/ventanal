// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
#include "../ventanal/owner.h"
#include <cassert>
#include <iostream>
int main() {
    PressClassifier single, twice, held, bounce;
    for(unsigned ms=0;ms<=600;ms++) {
        single.sample(ms,ms<80);
        twice.sample(ms,ms<80 || (ms>=250 && ms<350));
        held.sample(ms,true);
        bounce.sample(ms,ms<80 || (ms>=100 && ms<110) || (ms>=250 && ms<260));
    }
    assert(!single.isDouble && twice.isDouble && !held.isDouble && !bounce.isDouble);
    // Long eligibility ends on the first HIGH sample, even before debounce.
    auto classify=[](unsigned duration, auto pressed) {
        PressClassifier result;
        for(unsigned ms=0;ms<=duration;ms++) result.sample(ms,pressed(ms));
        return result;
    };
    assert(!classify(1499,[](unsigned){return true;}).isLong);
    assert(classify(1500,[](unsigned){return true;}).isLong);
    assert(classify(1600,[](unsigned ms){return ms<1600;}).isLong);
    assert(classify(5000,[](unsigned){return true;}).isLong);
    assert(!classify(1800,[](unsigned ms){return ms<1499;}).isLong);
    auto secondHold=classify(5000,[](unsigned ms){return ms<80 || ms>=250;});
    assert(secondHold.isDouble && !secondHold.isLong);
    auto bounced=classify(5000,[](unsigned ms){return ms!=100;});
    assert(!bounced.isDouble && !bounced.isLong);
    assert(!classify(1600,[](unsigned ms){return ms>=1;}).isLong);
    bool cached[3]={true,false,true};
    assert(nextSpare(-1,cached)==0 && nextSpare(0,cached)==2 && nextSpare(2,cached)==-1);
    OwnerState owner; strcpy(owner.mainId,"1111111111111111");
    strcpy(owner.overrideArt.id,"2222222222222222"); owner.cursor=1;
    owner.mainChanged("1111111111111111"); assert(owner.cursor==1 && owner.overrideArt.id[0]);
    owner.mainChanged("3333333333333333"); assert(owner.cursor==-1 && !owner.overrideArt.id[0]);
    SkipQueue queue;
    for(int i=0;i<10;i++) { char id[17]; snprintf(id,sizeof id,"%016x",i); queue.push(id,"aaaaaaaaaaaaaaaa"); }
    assert(queue.count==8 && !strcmp(queue.pairs[0].skipped,"0000000000000002"));
    char query[280]; queue.query(query,sizeof query);
    assert(strstr(query,"0000000000000002~aaaaaaaaaaaaaaaa"));
    queue.acknowledge(false); assert(queue.count==8);
    queue.acknowledge(true); assert(queue.count==0);
    assert(frameWakeRemainingMs(100000)==5000 && frameWakeRemainingMs(105000)==0);
    std::cout<<"owner: debounce, cycling, queue and download budget passed\n";
}
