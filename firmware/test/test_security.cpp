// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
#include "../ventanal/security.h"
#include "../ventanal/manifest_security.h"
#include "../lcd43b/transport.h"
#include <cassert>
#include <fstream>
#include <iostream>
int main() {
    std::ifstream input("fixtures/request-proof.json");
    assert(input);
    JsonDocument proof;
    assert(!deserializeJson(proof, input));
    assert(requestProof(std::string(32,'\0'), proof["method"].as<const char*>(),
                        proof["path"].as<const char*>(), proof["counter"].as<uint64_t>()) ==
           proof["mac"].as<const char*>());
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

    const std::string lcdKey(32, '\0');
    HTTPClient http;
    const auto lcdProof = proof["lcd"];
    const auto path = lcdProof["path"].as<const char*>();
    assert(!lcd::addPairedHeaders(http, lcdKey, "bad ID", path));
    assert(lcd::addPairedHeaders(http, lcdKey, "test-lcd", path));
    assert(lastHeaders.at("X-Ventanal-Counter") == "1");
    assert(lastHeaders.at("X-Ventanal-Device") == "test-lcd");
    assert(lcdSecurityWrites == 1 && lcd::pairingCounterFlash.exchange(false));
    failLcdSecurityWrite = true; // requests inside the reservation never touch flash
    assert(lcd::addPairedHeaders(http, lcdKey, "test-lcd", path));
    assert(lastHeaders.at("X-Ventanal-Counter") == std::to_string(lcdProof["counter"].as<uint64_t>()));
    assert(lastHeaders.at("Authorization") == std::string("Ventanal ") + lcdProof["mac"].as<const char*>());
    for (uint64_t expected = 3; expected <= 64; ++expected)
        assert(lcd::nextPairingCounter(lcdKey, counter) && counter == expected);
    assert(lcdSecurityWrites == 1 && !lcd::pairingCounterFlash.load());
    assert(!lcd::nextPairingCounter(lcdKey, counter) && counter == 64);
    failLcdSecurityWrite = false;
    assert(lcd::nextPairingCounter(lcdKey, counter) && counter == 65);
    assert(lcdSecurityWrites == 2 && lcd::pairingCounterFlash.exchange(false));

    lcd::PairingCounters afterBoot;
    assert(afterBoot.next(lcdKey, counter) && counter == 129);
    assert(lcdSecurityWrites == 3 && lcd::pairingCounterFlash.exchange(false));
    const std::string newKey(32, 'x');
    assert(afterBoot.next(newKey, counter) && counter == 1);
    assert(lcdSecurityWrites == 4 && lcd::pairingCounterFlash.exchange(false));
    assert(afterBoot.next(newKey, counter) && counter == 2 && lcdSecurityWrites == 4);

    lcdSecurityNvs["high-water"] = {1, 2, 3};
    lcd::PairingCounters corrupt;
    assert(!corrupt.next(newKey, counter) && lcdSecurityWrites == 4);
    assert(!lcd::pairingCounterFlash.load());

    // An upgraded board skips every value the legacy firmware may have sent.
    lcdSecurityNvs.clear();
    lcdSecurityNamespaceExists = false;
    struct { char fingerprint[64]; uint64_t highWater; } legacy{};
    const auto fingerprint = hmacHex(lcdKey, "Ventanal LCD replay counter");
    memcpy(legacy.fingerprint, fingerprint.data(), sizeof legacy.fingerprint);
    legacy.highWater = UINT64_MAX - 2;
    assert(p.putBytes("lcd-sec-counter", &legacy, sizeof legacy) == sizeof legacy);
    lcd::PairingCounters upgraded;
    assert(upgraded.next(lcdKey, counter) && counter == UINT64_MAX - 1);
    assert(upgraded.next(lcdKey, counter) && counter == UINT64_MAX);
    assert(!upgraded.next(lcdKey, counter) && lcdSecurityWrites == 5);
    assert(p.getBytes("lcd-sec-counter", &legacy, sizeof legacy) == sizeof legacy);
    assert(legacy.highWater == UINT64_MAX - 2);

    std::ifstream lcdInput("fixtures/lcd43b-signed-manifest.json");
    assert(lcdInput);
    const std::string body((std::istreambuf_iterator<char>(lcdInput)), {});
    lcd::DeviceManifest manifest;
    assert(!lcd::parseSignedDeviceManifest(body.c_str(), std::string(32, 'x'), manifest));
    assert(!manifest.screens[0].present);
    assert(lcd::parseSignedDeviceManifest(body.c_str(), lcdKey, manifest));
    assert(manifest.screens[0].present && manifest.screens[1].present);
    assert(!manifest.clock24h && manifest.quietStart == 1380);
    JsonDocument tampered;
    assert(!deserializeJson(tampered, body));
    tampered["screens"]["art"]["sha256"] = std::string(64, 'f');
    std::string changed;
    serializeJson(tampered, changed);
    assert(!lcd::parseSignedDeviceManifest(changed.c_str(), lcdKey, manifest));
    assert(strcmp(manifest.screens[0].sha256, std::string(64, '0').c_str()) == 0);
    tampered.remove("sig");
    changed.clear();
    serializeJson(tampered, changed);
    assert(!lcd::parseSignedDeviceManifest(changed.c_str(), lcdKey, manifest));
    std::cout << "security crypto, canonical JSON, durable counter and overflow passed\n";
}
