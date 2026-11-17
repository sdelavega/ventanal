// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
#pragma once
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
inline std::map<std::string,std::string> lastHeaders;
inline unsigned readDelay=0, streamTimeout=15000;
inline size_t largestRead = 0;
inline size_t readBytesCalls = 0;
class WiFiClient {
    size_t pos = 0;
public:
    void resetResponse() { pos=0; }
    void setTimeout(unsigned ms) { streamTimeout=ms; }
    int available() { return std::min(size_t(1460),response.size()-pos); }
    size_t readBytes(uint8_t* b, size_t n) { ++readBytesCalls; largestRead=std::max(largestRead,n); return read(b,n); }
    int read(uint8_t* b, size_t n) { fakeClock+=readDelay; largestRead=std::max(largestRead,n); n=std::min(n,response.size()-pos); std::memcpy(b,response.data()+pos,n); pos+=n; return n; }
    bool connected() { return pos < response.size(); }
};
class HTTPClient {
    WiFiClient* client = nullptr;
public:
    bool begin(WiFiClient& c, const char* url) { lastUrl=url; client=&c; c.resetResponse(); return !failBegin; }
    bool begin(WiFiClient& c, const String& u) { return begin(c, u.c_str()); }
    void setReuse(bool) {}
    void addHeader(const char* k,const char* v) { lastHeaders[k]=v; }
    String getString() { return responseBody; }
    void setTimeout(unsigned) {}
    void setConnectTimeout(unsigned) {}
    void setUserAgent(const char* value) { lastUserAgent=value; }
    int GET() { if(!responseBody.empty()) response.assign(responseBody.begin(),responseBody.end()); return status; }
    int getSize() { return advertisedSize; }
    WiFiClient* getStreamPtr() { return client; }
    bool connected() { return client->connected(); }
    void end() {}
};
