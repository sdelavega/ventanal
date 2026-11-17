// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
#pragma once
#include "security.h"
#include <ArduinoJson.h>
#include <algorithm>
// Manifest schema uses integer numbers and UTF-8 strings. Recursively sorted
// object keys, array order retained, compact ArduinoJson string escaping.
inline std::string canonicalJson(JsonVariantConst value) {
    std::string out;
    if(value.is<JsonObjectConst>()) {
        std::vector<std::string> keys;
        for(JsonPairConst pair:value.as<JsonObjectConst>()) keys.emplace_back(pair.key().c_str());
        std::sort(keys.begin(),keys.end()); out="{";
        for(const auto& key:keys) {
            if(out.size()>1) out+=',';
            JsonDocument name; name.set(key); std::string quoted; serializeJson(name,quoted); out+=quoted;
            out+=':'; out+=canonicalJson(value[key]);
        }
        out+='}';
    } else if(value.is<JsonArrayConst>()) {
        out="[";
        for(JsonVariantConst entry:value.as<JsonArrayConst>()) {
            if(out.size()>1) out+=',';
            out+=canonicalJson(entry);
        }
        out+=']';
    } else serializeJson(value,out);
    return out;
}
inline bool verifyManifest(JsonDocument& doc) {
    const std::string signature=doc["sig"] | "";
    if(!hex64(signature.c_str())) return false;
    doc.remove("sig");
    const auto expected=hmacHex(deviceKey(),canonicalJson(doc.as<JsonVariantConst>()));
    if(expected.size()!=64) return false;
    unsigned diff=0;
    for(unsigned i=0;i<64;++i) diff|=signature[i]^expected[i];
    if(diff) return false;
    std::vector<ExpectedHash> hashes;
    auto add=[&](JsonVariantConst entry) {
        const char* sha=entry["sha256"] | "";
        if(!hex64(sha)) return false;
        hashes.push_back({entry["url"] | "",sha}); return true;
    };
    for(JsonPairConst pair:doc["screens"].as<JsonObjectConst>()) if(!add(pair.value())) return false;
    for(JsonVariantConst entry:doc["spares"].as<JsonArrayConst>()) if(!add(entry)) return false;
    expectedHashes=hashes;
    return true;
}
