// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
#pragma once
#include "security.h"
#include "manifest_signature.h"

inline bool verifyManifest(JsonDocument& doc) {
    if (!verifyManifestSignature(doc, deviceKey())) return false;
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
