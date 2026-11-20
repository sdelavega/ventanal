// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
#include "../ventanal/manifest_security.h"
#include "../ventanal/net.h"
#include "../ventanal/battery.h"
#include "../ventanal/cache.h"
#include <Inkplate.h>
#include <cassert>
#include <iostream>
#include <fstream>
#include <sstream>
Inkplate display;
std::string sign(JsonDocument& doc) {
    doc.remove("sig");
    doc["sig"]=hmacHex(deviceKey(),canonicalJson(doc.as<JsonVariantConst>()));
    std::string result; serializeJson(doc,result); return result;
}
int main() {
    assert(securityConfigured());
    // Independent Python UTF-8 canonical-JSON/HMAC vector.
    JsonDocument vector;
    deserializeJson(vector,R"VECTOR({"version":1,"screens":{"art":{"url":"/frames/art.vnf","sha256":"0000000000000000000000000000000000000000000000000000000000000000","credit":{"title":"Árbol / \"x\" \n"}}},"sig":"62a960f037a0fdf4725525578e9351b1c04acc36305ede65dfca032ced36c12f"})VECTOR");
    assert(verifyManifest(vector));
    vector["screens"]["art"]["credit"]["title"]="tampered";
    assert(!verifyManifest(vector));
    std::ifstream controls("fixtures/manifest-controls.json"); assert(controls);
    std::stringstream controlContents; controlContents << controls.rdbuf();
    assert(!deserializeJson(vector, controlContents.str()));
    assert(verifyManifest(vector));
    TransportClient client; HTTPClient http;
    TransportClient otherClient; HTTPClient otherHttp;
    assert(&requestClient(client)==&requestClient(otherClient));
    assert(&requestHttp(http)==&requestHttp(otherHttp));
    assert(!beginTransport(http,client,"http://test:8766/manifest.json"));
    assert(!beginTransport(http,client,"https://test:8766.evil/manifest.json"));
    assert(beginTransport(http,client,"https://test:8766/manifest.json?bat=90&skips=a~b"));
    assert(lastHeaders["X-Ventanal-Counter"]=="1");
    assert(lastHeaders["Authorization"]=="Ventanal "+requestProof(deviceKey(),"GET","/manifest.json?bat=90&skips=a~b",1));
    auto before=lastHeaders["Authorization"];
    assert(beginTransport(http,client,"https://test:8766/manifest.json?bat=90&skips=a~b"));
    assert(lastHeaders["X-Ventanal-Counter"]=="2" && lastHeaders["Authorization"]!=before);
    response.assign(960016,0x25);
    const uint8_t header[16]={'V','N','F','1',0x40,6,0xb0,4,0,0,0,0,0,0,0,0};
    std::copy(header,header+16,response.begin());
    FrameHash hash; hash.add(response.data(),response.size()); auto sha=hash.finish();
    JsonDocument doc;
    deserializeJson(doc,R"({"version":1,"screens":{"art":{"id":"0123456789abcdef","url":"/frames/art.vnf","size":960016,"readout":{"x":0,"y":0,"w":10,"h":10,"style":"shade"}},"weather":{"id":"fedcba9876543210","url":"/frames/weather.vnf","size":960016,"readout":{"x":0,"y":0,"w":10,"h":10,"style":"lcars"}}}})");
    doc["screens"]["art"]["sha256"]=sha; doc["screens"]["weather"]["sha256"]=sha;
    doc["battery"]["full_mv"]=4144;
    auto body=sign(doc);
    responseBody=body; advertisedSize=body.size(); ScreenInfo a{},w{};
    assert(fetchManifest(a,w)); assert(expectedHashes.size()==2);
    assert(manifestBatteryFullMv==4144);
    doc["battery"]["full_mv"]=4200;
    serializeJson(doc,responseBody); advertisedSize=responseBody.size();
    assert(!fetchManifest(a,w) && manifestBatteryFullMv==4144);
    responseBody=body; advertisedSize=body.size();
    auto last=w;
    responseBody[responseBody.find("fedcba9876543210")]='a';
    assert(!fetchManifest(a,w)); assert(!memcmp(&last,&w,sizeof w));
    responseBody.clear();
    response.assign(960016,0x25); std::copy(header,header+16,response.begin()); advertisedSize=response.size();
    constexpr char artUrl[] = "https://test:8766/frames/art.vnf";
    urlResponses[artUrl] = response;
    persistentServer = true;
    sizeFromResponse = true;
    fakeGetResponse = [] {
        assert(lastConnectionHeader == "keep-alive");
        const auto counter = std::stoull(lastHeaders.at("X-Ventanal-Counter"));
        assert(lastHeaders.at("Authorization") == "Ventanal " +
            requestProof(deviceKey(), "GET", "/frames/art.vnf", counter));
        const auto& range = lastHeaders.at("Range");
        if (range == "bytes=0-32767") {
            assert(!lastHeaders.count("If-Range"));
            fakeResponseHeaders["ETag"] = "\"0123456789abcdef\"";
            fakeResponseHeaders["Content-Range"] = "bytes 0-32767/960016";
            response.resize(32768);
            return 206;
        }
        assert(range == "bytes=32768-65535" && lastHeaders.at("If-Range") == "\"0123456789abcdef\"");
        return 200;  // A complete body replaces the checkpoint from offset zero.
    };
    const auto requested = requestedUrls.size();
    const auto logStart = Serial.output.size();
    assert(fetchFrame(artUrl,"/art.vnf"));
    assert(Serial.output.find("security frame-hash=verified", logStart) != std::string::npos);
    assert(requestedUrls.size() == requested + 2);
    fakeGetResponse = nullptr;
    sizeFromResponse = false;
    persistentServer = false;
    urlResponses.erase(artUrl);
    auto good=files["/art.vnf"];
    response.back()=0x24; // Valid VNF indices, invalid signed digest.
    assert(!fetchFrame("https://test:8766/frames/art.vnf","/art.vnf"));
    assert(files["/art.vnf"]==good && !files.count("/art.vnf.tmp"));
    assert(!fetchFrame("https://test:8766/frames/unknown.vnf","/art.vnf"));
    assert(files["/art.vnf"]==good);
    doc["screens"]["solar"].set(doc["screens"]["weather"]);
    doc["screens"]["solar"]["url"]="/frames/solar.vnf";
    responseBody=sign(doc); advertisedSize=responseBody.size();
    assert(fetchManifest(a,w) && expectedHashes.size()==3);
    assert(manifestExtras.solar.id[0]);
    responseBody.clear();
    response.assign(960016,0x25); std::copy(header,header+16,response.begin()); advertisedSize=response.size();
    assert(fetchFrame("https://test:8766/frames/solar.vnf","/solar.vnf"));
    auto solarGood=files["/solar.vnf"];
    response.back()=0x24;
    assert(!fetchFrame("https://test:8766/frames/solar.vnf","/solar.vnf"));
    assert(files["/solar.vnf"]==solarGood);
    responseBody=sign(doc); advertisedSize=responseBody.size();
    auto solarAt=responseBody.find("/frames/solar.vnf"); assert(solarAt!=std::string::npos);
    responseBody[solarAt+8]='x';
    assert(!fetchManifest(a,w));
    // Extras and the ordered cycle are covered by the same signed document.
    doc["screens"]["extra-test"].set(doc["screens"]["weather"]);
    doc["screens"]["extra-test"]["url"]="/frames/extra-test.vnf";
    auto order=doc["screen_order"].to<JsonArray>();
    for(const char* name:{"art","weather","solar","extra-test"}) order.add(name);
    responseBody=sign(doc); advertisedSize=responseBody.size();
    assert(fetchManifest(a,w) && manifestExtras.orderCount==4);
    assert(manifestExtras.order[3]==Screen::Extra0 && !strcmp(manifestExtras.names[0],"extra-test"));
    order[1]="extra-test";
    serializeJson(doc,responseBody); advertisedSize=responseBody.size();
    assert(!fetchManifest(a,w)); // Reordering without resigning is rejected.
    responseBody=sign(doc); advertisedSize=responseBody.size();
    assert(!fetchManifest(a,w)); // Even signed duplicate/omitted names are invalid.
    order[1]="weather";
    doc["screens"]["extra-second"].set(doc["screens"]["extra-test"]); order.add("extra-second");
    responseBody=sign(doc); advertisedSize=responseBody.size();
    assert(fetchManifest(a,w) && manifestExtras.orderCount==5 && manifestExtras.order[4]==Screen::Extra1);
    order.add("extra-third");
    responseBody=sign(doc); advertisedSize=responseBody.size();
    assert(!fetchManifest(a,w));
    // Generated through the real Python test extra's entitlement/publication path.
    std::ifstream input("fixtures/extras-server-manifest.json"); assert(input);
    std::stringstream contents; contents << input.rdbuf();
    JsonDocument serverVector; assert(!deserializeJson(serverVector,contents.str()));
    responseBody=sign(serverVector); advertisedSize=responseBody.size();
    assert(fetchManifest(a,w) && manifestExtras.orderCount==4);
    assert(manifestExtras.order[3]==Screen::Extra0 && !strcmp(manifestExtras.names[0],"extra-test"));
    assert(!strcmp(manifestExtras.extraScreens[0].id,"8cc1cf86d93ff0d6"));
    std::cout << "secure transport: proofs, no HTTP downgrade, signed manifest tamper rejection and last-good frame hashes passed\n";
}
