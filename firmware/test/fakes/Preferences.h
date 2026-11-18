// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
#pragma once
#include "LittleFS.h"
inline std::map<std::string,std::vector<uint8_t>> preferences;
inline bool preferencesNamespaceExists=false;
class Preferences {
    bool opened = false;
public:
    inline static bool failBegin = false;
    inline static bool failStateRead = false;
    inline static std::string failWriteKey;
    inline static int stateReadFailures = 0;
    inline static int stateReads = 0;
    bool isKey(const char* k) { return opened && preferences.count(k); }
    bool begin(const char*,bool readOnly=false) {
        opened=!failBegin && (!readOnly || preferencesNamespaceExists);
        if(opened && !readOnly) preferencesNamespaceExists=true;
        return opened;
    }
    void end() { opened=false; }
    size_t getBytesLength(const char* k) { return isKey(k) ? preferences.at(k).size() : 0; }
    size_t getBytes(const char* k,void* p,size_t n) {
        if(std::string(k)=="state13") {
            ++stateReads;
            if(stateReadFailures>0) { --stateReadFailures; return 0; }
        }
        if(!isKey(k) || (failStateRead && std::string(k)=="state13") || preferences.at(k).size()!=n) return 0;
        memcpy(p,preferences.at(k).data(),n); return n;
    }
    size_t putBytes(const char* k,const void* p,size_t n) {
        if(!opened || failWriteKey==k) return 0;
        const auto* b=static_cast<const uint8_t*>(p); preferences[k]={b,b+n}; return n;
    }
};
