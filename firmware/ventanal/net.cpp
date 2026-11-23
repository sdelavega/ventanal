// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
#include "net.h"
#include "quiet.h"
#include "settings.h"
#include <ArduinoJson.h>
#include <algorithm>
#include "http.h"
#include "manifest_security.h"
#include "battery.h"
#include <Inkplate.h>
#include <WiFi.h>
#include <esp_sntp.h>
#include <sys/time.h>

extern Inkplate display;

bool wifiConnect(uint32_t timeoutMs) {
    WiFi.persistent(false); // The settings blob is the only persistent credential source.
    WiFi.mode(WIFI_STA);
    WiFi.begin(installation.ssid, installation.password);
    const uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < timeoutMs) delay(50);
    const bool connected = WiFi.status() == WL_CONNECTED;
    Serial.printf("wifi connected=%d\n", connected);
    return connected;
}

time_t seedRtcTime() {
    // RTC stores UTC. Library epoch helpers use the process TZ internally.
    // Temporarily select UTC so DST transitions never make the RTC ambiguous.
    setenv("TZ", "UTC0", 1);
    tzset();
    time_t seeded = 0;
    if (display.rtc.isSet()) {
        const time_t epoch = display.rtc.getEpoch();
        if (epoch >= 1704067200) {
            timeval tv{epoch, 0};
            if (settimeofday(&tv, nullptr) == 0) {
                seeded = epoch;
                Serial.printf("time seeded=rtc\n");
            }
        }
    }
    setenv("TZ", installation.timezone, 1);
    tzset();
    return seeded;
}

bool syncTime() {
    seedRtcTime();
    if (WiFi.status() != WL_CONNECTED) return false;
    configTzTime(installation.timezone, "pool.ntp.org", "time.nist.gov");
    const uint32_t start = millis();
    bool synced = false;
    // The IDF getter consumes COMPLETED and resets the status. Read it once.
    while (millis() - start < 15000) {
        if (sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED) {
            synced = true;
            break;
        }
        delay(100);
    }
    if (!synced) {
        Serial.printf("time ntp=unavailable\n");
        return false;
    }
    setenv("TZ", "UTC0", 1);
    tzset();
    display.rtc.setEpoch(static_cast<uint32_t>(time(nullptr)));
    setenv("TZ", installation.timezone, 1);
    tzset();
    Serial.printf("time ntp=synced rtc=written\n");
    return true;
}

namespace {
bool parseScreen(JsonVariantConst value, ScreenInfo& result) {
    const char* id = value["id"] | "";
    const char* url = value["url"] | "";
    // Raw frames are exactly 960016 bytes. A zlib wire names its own size
    // and must carry the decoded frame's SHA-256, which the download verifies.
    const char* encoding = value["encoding"] | "";
    const uint32_t size = value["size"].is<uint32_t>() ? value["size"].as<uint32_t>() : 0;
    const bool zlib = !strcmp(encoding, "zlib");
    if (!zlib && !value["encoding"].isNull()) return false;
    if (zlib ? size < 8 || size > maxZlibFrameBytes || !hex64(value["sha256"] | "") : size != 960016)
        return false;
    if (strlen(id) != 16 || strlen(url) >= sizeof(result.url) ||
        strncmp(url, "/frames/", 8) != 0 || strstr(url, "..")) return false;
    for (unsigned i = 0; i < 16; ++i)
        if (!((id[i] >= '0' && id[i] <= '9') || (id[i] >= 'a' && id[i] <= 'f'))) return false;
    JsonVariantConst r = value["readout"];
    if (!r["x"].is<int>() || !r["y"].is<int>() || !r["w"].is<int>() || !r["h"].is<int>()) return false;
    int x = r["x"], y = r["y"], w = r["w"], h = r["h"];
    if (x < 0 || y < 0 || w <= 0 || h <= 0 || w > 1600 || h > 1200 || x > 1600-w || y > 1200-h) return false;
    const char* style = r["style"] | "";
    if (strcmp(style, "lcars") && strcmp(style, "shade")) return false;
    strcpy(result.id, id);
    strcpy(result.url, url);
    result.readout = {static_cast<int16_t>(x), static_cast<int16_t>(y),
                      static_cast<int16_t>(w), static_cast<int16_t>(h), !strcmp(style, "lcars")};
    return true;
}
}

bool fetchManifest(ScreenInfo& art, ScreenInfo& weather, bool* transient) {
    if (transient) *transient = false;
    manifestFirmware.release[0] = '\0';
    manifestFirmware.signature[0] = '\0';
    manifestFirmware.path[0] = '\0';
    manifestFirmware.approval[0] = '\0';
    TransportClient localClient;
    auto& client=requestClient(localClient);
    HTTPClient localHttp;
    auto& http=requestHttp(localHttp);
    configureHttp(http);
    String url = String(installation.host) + "/manifest.json";
    if(manifestBoardQuery[0]) url += String("?")+manifestBoardQuery;
    if(manifestSkipQuery[0]) url += String(manifestBoardQuery[0]?"&skips=":"?skips=")+manifestSkipQuery;
    if (!beginTransport(http, client, url.c_str())) return false;
    const uint32_t started=millis();
    const int status = http.GET();
    Serial.printf("transport tls=%d handshake-headers ms=%lu status=%d\n",securityConfigured(),static_cast<unsigned long>(millis()-started),status);
    const int size = http.getSize();
    if (transient) *transient = status == HTTPC_ERROR_CONNECTION_REFUSED
        || status == HTTPC_ERROR_NOT_CONNECTED || status == HTTPC_ERROR_CONNECTION_LOST
        || status == HTTPC_ERROR_READ_TIMEOUT
        || status == 502 || status == 503 || status == 504;
    if (status != 200 || size <= 0 || size > 8192) { finishTransport(http,false); return false; }
    String body;
    auto* stream=http.getStreamPtr();
    char block[257];
    uint32_t lastProgress=millis();
    while(body.length() < static_cast<size_t>(size) && millis()-started < 30000) {
        const int available=stream->available();
        if(available > 0) {
            const size_t n=std::min({size_t(available), sizeof(block)-1,
                                    static_cast<size_t>(size)-body.length()});
            const int got=stream->read(reinterpret_cast<uint8_t*>(block),n);
            if(got<=0) break;
            block[got]='\0';
            body+=block;
            lastProgress=millis();
        } else {
            if(!http.connected() || millis()-lastProgress>=15000) break;
            delay(1);
        }
    }
    finishTransport(http,body.length()==static_cast<size_t>(size) && millis()-started<30000);
    if (millis()-started >= 30000 || body.length() != static_cast<size_t>(size)) {
        if (transient) *transient = true;
        return false;
    }
    JsonDocument doc;
    if (deserializeJson(doc, body) || (doc["version"] | 0) != 1) return false;
    if(securityConfigured() && !verifyManifest(doc)) {
        Serial.println("security manifest=rejected"); return false;
    }
    if(securityConfigured()) Serial.println("security manifest=verified");
    ScreenInfo a{}, w{};
    if (!parseScreen(doc["screens"]["art"], a) || !parseScreen(doc["screens"]["weather"], w)) return false;
    ManifestExtras extras;
    if(!doc["screens"]["solar"].isNull() && !parseScreen(doc["screens"]["solar"],extras.solar)) return false;
    // Old manifests retain the core order; new manifests explicitly carry it.
    auto add=[&](const char* name) {
        if(extras.orderCount>=5) return false;
        Screen slot;
        if(!strcmp(name,"art")) slot=Screen::Art;
        else if(!strcmp(name,"weather")) slot=Screen::Weather;
        else if(!strcmp(name,"solar") && extras.solar.id[0]) slot=Screen::Solar;
        else {
            if(strncmp(name,"extra-",6) || strlen(name)>48 || !name[6]) return false;
            for(const char* c=name;*c;++c) if(!(*c>='a' && *c<='z') && !(*c>='0' && *c<='9') && *c!='-' && *c!='_') return false;
            if(!strcmp(name,extras.names[0]) || !strcmp(name,extras.names[1])) return false;
            unsigned i=extras.names[0][0]?1:0;
            if(extras.names[i][0] || !parseScreen(doc["screens"][name],extras.extraScreens[i])) return false;
            strcpy(extras.names[i],name); slot=i?Screen::Extra1:Screen::Extra0;
        }
        for(unsigned i=0;i<extras.orderCount;++i) if(extras.order[i]==slot) return false;
        extras.order[extras.orderCount++]=slot; return true;
    };
    if(doc["screen_order"].isNull()) {
        add("art"); add("weather"); if(extras.solar.id[0]) add("solar");
        if(doc["screens"].size()!=extras.orderCount) return false;
    } else {
        if(!doc["screen_order"].is<JsonArrayConst>() || doc["screen_order"].size()<2 || doc["screen_order"].size()>5) return false;
        for(JsonVariantConst name:doc["screen_order"].as<JsonArrayConst>()) {
            if(!name.is<const char*>() || !add(name.as<const char*>())) return false;
        }
        bool artFound=false,weatherFound=false;
        for(unsigned i=0;i<extras.orderCount;++i) { artFound|=extras.order[i]==Screen::Art; weatherFound|=extras.order[i]==Screen::Weather; }
        if(!artFound || !weatherFound || doc["screens"].size()!=extras.orderCount) return false;
    }
    if(!doc["spares"].isNull()) {
        if(!doc["spares"].is<JsonArrayConst>() || doc["spares"].size()>3) return false;
        for(JsonVariantConst value : doc["spares"].as<JsonArrayConst>()) {
            if(!parseScreen(value,extras.spares[extras.count])) return false;
            ++extras.count;
        }
    }
    if(!doc["quiet"].isNull()) {
        QuietHours q{parseMinute(doc["quiet"]["start"] | ""), parseMinute(doc["quiet"]["end"] | "")};
        if(!q.valid()) return false;
        quietHours=q;
    }
    // Commit only after all manifest validation, including its signature.
    // Invalid calibration is ignored; absence explicitly restores the default.
    if(doc["battery"].isNull()) manifestBatteryFullMv=0;
    else if(doc["battery"]["full_mv"].is<int>() && validBatteryFull(doc["battery"]["full_mv"].as<int>()))
        manifestBatteryFullMv=doc["battery"]["full_mv"].as<int>();
    else manifestBatteryFullMv=batteryFullMv;
    const auto firmware = doc["firmware"];
    auto copyField = [&](const char* key, char* dst, size_t capacity) {
        const auto value = firmware[key].as<JsonString>();
        if (value.isNull() || !value.size() || value.size() >= capacity
            || strlen(value.c_str()) != value.size()) return false;
        memcpy(dst, value.c_str(), value.size() + 1);
        return true;
    };
    if (copyField("release", manifestFirmware.release, sizeof manifestFirmware.release)
        && (!copyField("sig", manifestFirmware.signature, sizeof manifestFirmware.signature)
            || !copyField("path", manifestFirmware.path, sizeof manifestFirmware.path)))
        manifestFirmware.release[0] = '\0';
    if (!copyField("approval", manifestFirmware.approval, sizeof manifestFirmware.approval)
        || !validOtaApproval(manifestFirmware.approval)) manifestFirmware.approval[0] = '\0';
    if(!securityConfigured()) rememberManifestFrames(doc, false);
    manifestExtras=extras;
    art = a;
    weather = w;
    return true;
}
