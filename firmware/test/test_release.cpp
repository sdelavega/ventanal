// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
#include <ArduinoJson.h>
#include <cassert>
#include <fstream>
#include <iostream>

#include "../common/release.h"

static const char* verdictName(ReleaseVerdict verdict) {
    switch (verdict) {
        case ReleaseVerdict::Ok: return "Ok";
        case ReleaseVerdict::BadSignature: return "BadSignature";
        case ReleaseVerdict::WrongBoard: return "WrongBoard";
        case ReleaseVerdict::NotNewer: return "NotNewer";
        case ReleaseVerdict::TooOld: return "TooOld";
        case ReleaseVerdict::BadSize: return "BadSize";
    }
    assert(false);
    return "";
}

int main() {
    const char* fixture = "../../server/tests/fixtures/release/vectors.json";
    std::ifstream input(fixture);
    if (!input) {
        std::cerr << "Could not open release vectors fixture: " << fixture
                  << " (run from firmware/test)\n";
        return 1;
    }
    JsonDocument vectors;
    assert(!deserializeJson(vectors, input));
    const auto cases = vectors.as<JsonArrayConst>();
    assert(cases.size() >= 10);
    for (JsonObjectConst test : cases) {
        const auto metadata = test["metadata"].as<JsonString>();
        const char* signature = test["signature"];
        const char* board = test["board"];
        const char* running = test["running"];
        ReleaseInfo info{};
        info.size = 123;
        const auto verdict = checkRelease(metadata.c_str(), metadata.size(), signature,
                                          board, running, test["slot_size"], info);
        const char* expected = test["verdict"];
        if (strcmp(verdictName(verdict), expected) != 0) {
            std::cerr << test["name"].as<const char*>() << ": expected " << expected
                      << ", got " << verdictName(verdict) << '\n';
            return 1;
        }
        if (verdict == ReleaseVerdict::Ok) {
            assert(strcmp(info.board, board) == 0);
            const auto image = test["image"].as<JsonString>();
            if (!image.isNull()) {
                uint8_t digest[crypto_hash_sha256_BYTES];
                assert(crypto_hash_sha256(digest, reinterpret_cast<const uint8_t*>(image.c_str()),
                                          image.size()) == 0);
                assert(releaseImageMatches(info, image.size(), digest)
                       == test["image_matches"].as<bool>());
            }
        } else {
            assert(info.size == 123);
        }
    }
    std::cout << cases.size() << " release vectors passed\n";
}
