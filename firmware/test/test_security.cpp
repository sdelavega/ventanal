// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
#include "../ventanal/security.h"
#include "../ventanal/manifest_security.h"
#include <cassert>
#include <iostream>
int main() {
    assert(hmacHex(std::string(32,'\0'),"GET/manifest.json1")=="58fe2f37b96dc6ea87bab1b479e17ffe76a051a3b327ec651026553d7bf361bb");
    uint64_t counter=0;
    assert(nextCounter(counter) && counter==1);
    assert(nextCounter(counter) && counter==2);
    Preferences p; p.begin("ventanal",false); uint64_t saved=0;
    assert(p.getBytes("sec-counter",&saved,sizeof saved)==sizeof saved && saved==2);
    saved=UINT64_MAX; p.putBytes("sec-counter",&saved,sizeof saved);
    assert(!nextCounter(counter));
    preferences["sec-counter"]={1,2,3};
    assert(!nextCounter(counter));
    preferences.erase("sec-counter");
    Preferences::failBegin=true; assert(!nextCounter(counter));
    Preferences::failBegin=false;
    JsonDocument doc; deserializeJson(doc,"{\"z\":1,\"a\":{\"title\":\"Árbol\",\"list\":[2,null,true]}}");
    assert(canonicalJson(doc.as<JsonVariantConst>())=="{\"a\":{\"list\":[2,null,true],\"title\":\"Árbol\"},\"z\":1}");
    assert(!verifyManifest(doc));
    std::cout << "security crypto, canonical JSON, durable counter and overflow passed\n";
}
