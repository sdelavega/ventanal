// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
#pragma once
inline constexpr int HTTPC_ERROR_CONNECTION_REFUSED=-1, HTTPC_ERROR_NOT_CONNECTED=-4,
    HTTPC_ERROR_CONNECTION_LOST=-5, HTTPC_ERROR_READ_TIMEOUT=-11;
inline constexpr int HTTPC_DISABLE_FOLLOW_REDIRECTS = 0;
#include "Arduino.h"
#include <vector>
#include <map>
#include <cstring>
inline std::vector<uint8_t> response;
inline int advertisedSize = 960016;
inline int status = 200;
inline bool failBegin = false;
inline std::string responseBody;
inline std::string lastUserAgent;
inline std::string lastUrl;
inline std::vector<std::string> requestedUrls;
inline std::map<std::string,std::string> lastHeaders;
inline unsigned readDelay=0, streamTimeout=15000;
inline size_t largestRead = 0;
inline size_t readBytesCalls = 0;
inline size_t frameReadCalls = 0;
inline size_t frameBytesRead = 0;
inline int (*fakeGetResponse)() = nullptr;
inline std::map<std::string, std::string> fakeResponseHeaders;
// lcd43b sync test additions (defaults keep the e-paper tests unchanged):
inline std::map<std::string,std::vector<uint8_t>> urlResponses; // per-URL body
inline bool sizeFromResponse = false;  // getSize() reports the body size
inline bool stallBody = false;         // connected, but no bytes ever arrive
inline size_t delayedFrom = 0;          // bytes at/after this index only arrive
inline unsigned delayedUntilMs = 0;     // once fakeClock passes this mark
inline bool holdConnection = false;     // connected() stays true (no hang-up)
// lcd43b connection-close framing (defaults keep every other test unchanged):
inline bool reuse = true;               // keep-alive, the installed core default
inline std::string lastConnectionHeader;   // Connection: header the request carries
inline bool socketTestMode = false, socketOpen = false;
inline size_t fakeHeaderBytes = 0;
inline bool persistentServer = false;   // HTTP/1.1 server: keeps the socket open
                                          // after the body ONLY under keep-alive
class WiFiClient {
    size_t pos = 0;
public:
    void resetResponse() { pos=0; }
    void setTimeout(unsigned ms) { streamTimeout=ms; }
    int available() {
        if (stallBody) return 0;
        const size_t remaining = response.size() - pos;
        if (!remaining) return 0;
        // A trailing packet that shows up late (after the frame) models the
        // delayed-excess case: nothing to read yet, connection still open.
        if (delayedUntilMs && pos >= delayedFrom && fakeClock < delayedUntilMs)
            return 0;
        return std::min(size_t(1460), remaining);
    }
    size_t readBytes(uint8_t* b, size_t n) { ++readBytesCalls; largestRead=std::max(largestRead,n); return read(b,n); }
    virtual int read(uint8_t* b, size_t n) { ++frameReadCalls; fakeClock+=readDelay; largestRead=std::max(largestRead,n); n=std::min(n,response.size()-pos); std::memcpy(b,response.data()+pos,n); pos+=n; frameBytesRead+=n; return n; }
    virtual int read() { uint8_t byte; return read(&byte, 1) == 1 ? byte : -1; }
    void stop() { socketOpen = false; }
    bool connected() { if (socketTestMode) return socketOpen; if (stallBody || holdConnection) return true; if (pos < response.size()) return true; return persistentServer && reuse; }
};
class HTTPClient {
    WiFiClient* client = nullptr;
    WiFiClient inlineClient;   // body for the single-argument begin()
public:
    bool begin(WiFiClient& c, const char* url) {
        lastHeaders.erase("Range");
        lastHeaders.erase("If-Range");
        lastUrl=url; client=&c; c.resetResponse(); return !failBegin;
    }
    bool begin(WiFiClient& c, const String& u) { return begin(c, u.c_str()); }
    bool begin(const char* url) { return begin(inlineClient, url); }
    bool begin(const String& u) { return begin(u.c_str()); }
    void setReuse(bool r) { reuse = r; }   // the production code disables keep-alive
    void addHeader(const char* k,const char* v) { lastHeaders[k]=v; }
    String getString() { return responseBody; }
    void setTimeout(unsigned) {}
    void setConnectTimeout(unsigned) {}
    void setFollowRedirects(int) {}
    void setUserAgent(const char* value) { lastUserAgent=value; }
    void collectHeaders(const char*[], size_t) {}
    String header(const char* key) {
        const auto found = fakeResponseHeaders.find(key);
        return found == fakeResponseHeaders.end() ? String() : String(found->second.c_str());
    }
    int GET() {
        requestedUrls.push_back(lastUrl);
        auto perUrl = urlResponses.find(lastUrl);
        if (perUrl != urlResponses.end()) response = perUrl->second;
        else if(!responseBody.empty()) response.assign(responseBody.begin(),responseBody.end());
        // The request line the real client would put on the wire: without
        // setReuse(false) this is "keep-alive" and a persistent server never
        // hangs up (the round-4 framing bug).
        lastConnectionHeader = reuse ? "keep-alive" : "close";
        lastHeaders["Connection"] = lastConnectionHeader;
        fakeResponseHeaders.clear();
        const int result = fakeGetResponse ? fakeGetResponse() : status;
        if (socketTestMode) {
            socketOpen = true;
            for (size_t i = 0; i < fakeHeaderBytes; ++i) client->read();
            client->resetResponse();
        }
        return result;
    }
    int getSize() { return sizeFromResponse ? int(response.size()) : advertisedSize; }
    WiFiClient* getStreamPtr() { return client; }
    bool connected() { return client->connected(); }
    void end() { if (socketTestMode && !reuse) socketOpen = false; }
};
