// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
#pragma once
#include "settings.h"
#include "security_protocol.h"
#include <Preferences.h>
#include <HTTPClient.h>
#include <string>
#include <cstring>
#include <vector>
#ifdef VENTANAL_HOST_TEST
#include <CommonCrypto/CommonDigest.h>
#else
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <mbedtls/sha256.h>
#endif

inline std::string deviceKey() { return decodeDeviceKey(installation.deviceKey); }
inline bool securityConfigured() {
    return (strlen(installation.certificate) + strlen(installation.deviceKey) > 0) ||
           strncmp(installation.host,"https://",8)==0;
}
struct FrameHash {
#ifdef VENTANAL_HOST_TEST
    CC_SHA256_CTX ctx;
    FrameHash() { CC_SHA256_Init(&ctx); }
    void add(const void* p,size_t n) { CC_SHA256_Update(&ctx,p,static_cast<CC_LONG>(n)); }
    std::string finish() { unsigned char out[32]; CC_SHA256_Final(out,&ctx); return hexBytes(out,32); }
#else
    mbedtls_sha256_context ctx;
    FrameHash() { mbedtls_sha256_init(&ctx); mbedtls_sha256_starts(&ctx,0); }
    ~FrameHash() { mbedtls_sha256_free(&ctx); }
    void add(const void* p,size_t n) { mbedtls_sha256_update(&ctx,static_cast<const unsigned char*>(p),n); }
    std::string finish() { unsigned char out[32]; mbedtls_sha256_finish(&ctx,out); return hexBytes(out,32); }
#endif
};
inline bool nextCounter(uint64_t& value) {
    Preferences p;
    if(!p.begin("ventanal",false)) return false;
    uint64_t old=0;
    // Missing key is initial pairing; malformed existing state fails closed.
    bool ok=!p.isKey("sec-counter") || p.getBytes("sec-counter",&old,sizeof old)==sizeof old;
    if(!ok || old==UINT64_MAX) { p.end(); return false; }
    value=old+1;
    ok=p.putBytes("sec-counter",&value,sizeof value)==sizeof value;
    p.end(); return ok;
}
// Count response bytes consumed by HTTPClient as well as body reads. In
// particular, GET() can fail after consuming an incomplete response header.
#ifdef VENTANAL_HOST_TEST
using SecureClientBase = WiFiClient;
#else
using SecureClientBase = WiFiClientSecure;
#endif
struct CountingSecureClient : SecureClientBase {
    size_t received = 0;
    int read() override {
        const int value = SecureClientBase::read();
        if (value >= 0) ++received;
        return value;
    }
    int read(uint8_t* buffer, size_t size) override {
        const int count = SecureClientBase::read(buffer, size);
        if (count > 0) received += count;
        return count;
    }
};
#ifdef VENTANAL_HOST_TEST
using TransportClient = CountingSecureClient;
inline CountingSecureClient& secureTransportClient(TransportClient& client) { return client; }
#else
struct TransportClient { WiFiClient plain; CountingSecureClient secure; };
inline CountingSecureClient& secureTransportClient(TransportClient& client) { return client.secure; }
#endif
// Only one Arduino task performs requests. Keep its verified TLS connection
// across manifest/main/weather and post-draw spare refills within this wake.
inline TransportClient& requestClient(TransportClient& local) {
    if(!securityConfigured()) return local;
    static TransportClient shared; return shared;
}
inline HTTPClient& requestHttp(HTTPClient& local) {
    if(!securityConfigured()) return local;
    static HTTPClient shared; return shared;
}
inline void finishTransport(HTTPClient& http,bool complete) {
    if(securityConfigured()) http.setReuse(complete);
    http.end();
}
inline bool beginTransport(HTTPClient& http,TransportClient& client,const char* url) {
    if(!securityConfigured()) {
        if(strncmp(url,"http://",7)!=0) return false;
#ifdef VENTANAL_HOST_TEST
        return http.begin(client,url);
#else
        return http.begin(client.plain,url);
#endif
    }
    const std::string key=deviceKey();
    const std::string origin=installation.host;
    if(key.size()!=32 || !strlen(installation.certificate) || origin.rfind("https://",0)!=0 ||
       std::string(url).compare(0,origin.size(),origin)!=0 || url[origin.size()]!='/') return false;
#ifndef VENTANAL_HOST_TEST
    client.secure.setCACert(installation.certificate);
    client.secure.setHandshakeTimeout(5);
#endif
#ifdef VENTANAL_HOST_TEST
    if(!http.begin(client,url)) return false;
#else
    if(!http.begin(client.secure,url)) return false;
#endif
    uint64_t counter;
    if(!nextCounter(counter)) { finishTransport(http,false); return false; }
    char number[21]; snprintf(number,sizeof number,"%llu",static_cast<unsigned long long>(counter));
    const auto proof=requestProof(key,"GET",url+origin.size(),counter);
    http.addHeader("Authorization",(std::string("Ventanal ")+proof).c_str());
    http.addHeader("X-Ventanal-Counter",number);
    http.setReuse(true);
    return true;
}
// Verified manifest hashes live only for this wake, never change NVS State ABI.
// sha is the decoded frame's SHA-256; a zlib entry also names its wire size.
struct ExpectedHash { std::string url, sha, id = {}; bool zlib = false; uint32_t wireSize = 0; };
inline std::vector<ExpectedHash> expectedHashes;
inline const ExpectedHash* expectedFrame(const char* url) {
    const std::string origin=installation.host;
    if(std::string(url).compare(0,origin.size(),origin)!=0) return nullptr;
    for(const auto& h:expectedHashes) if(h.url==url+origin.size()) return &h;
    return nullptr;
}
