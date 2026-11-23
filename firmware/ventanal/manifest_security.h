// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
#pragma once
#include "security.h"
#include "manifest_signature.h"

inline bool rememberManifestFrames(JsonDocument& doc, bool requireHash) {
    std::vector<ExpectedHash> hashes;
    auto add=[&](JsonVariantConst entry) {
        const char* sha=entry["sha256"] | "";
        if(!hex64(sha)) {
            if(requireHash) return false;
            sha="";
        }
        const bool zlib=!strcmp(entry["encoding"] | "","zlib");
        hashes.push_back({entry["url"] | "",sha,entry["id"] | "",zlib,zlib ? entry["size"].as<uint32_t>() : 0});
        return true;
    };
    for(JsonPairConst pair:doc["screens"].as<JsonObjectConst>()) if(!add(pair.value())) return false;
    for(JsonVariantConst entry:doc["spares"].as<JsonArrayConst>()) if(!add(entry)) return false;
    expectedHashes=std::move(hashes);
    return true;
}

inline bool verifyManifest(JsonDocument& doc) {
    return verifyManifestSignature(doc, deviceKey()) && rememberManifestFrames(doc, true);
}
