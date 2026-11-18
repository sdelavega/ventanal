// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
#pragma once
#include "Preferences.h"
using nvs_handle_t = int;
using nvs_type_t = int;
constexpr int ESP_OK=0;
constexpr int NVS_READWRITE=1;
constexpr int NVS_READONLY=0, ESP_ERR_NVS_NOT_FOUND=1, ESP_ERR_NO_MEM=2;
inline bool failRawStateRead=false, failQuarantineWrite=false, failKeyLookup=false, failNvsOpen=false;
inline int nvs_open(const char*,int mode,nvs_handle_t* handle) {
    if(failNvsOpen) return ESP_ERR_NO_MEM;
    if(mode==NVS_READONLY && !preferencesNamespaceExists) return ESP_ERR_NVS_NOT_FOUND;
    if(mode==NVS_READWRITE) preferencesNamespaceExists=true;
    *handle=1;
    return ESP_OK;
}
inline int nvs_find_key(nvs_handle_t,const char* key,nvs_type_t*) {
    return failKeyLookup ? -1 : preferences.count(key) ? ESP_OK : ESP_ERR_NVS_NOT_FOUND;
}
inline int nvs_get_blob(nvs_handle_t,const char* key,void* bytes,size_t* length) {
    if(failRawStateRead || !preferences.count(key) || *length<preferences[key].size()) return -1;
    *length=preferences[key].size();
    if(*length) memcpy(bytes,preferences[key].data(),*length);
    return 0;
}
inline int nvs_set_blob(nvs_handle_t,const char* key,const void* bytes,size_t length) {
    if(failQuarantineWrite) return -1;
    const auto* raw=static_cast<const uint8_t*>(bytes);
    preferences[key]={raw,raw+length}; return 0;
}
inline int nvs_commit(nvs_handle_t) { return 0; }
inline void nvs_close(nvs_handle_t) {}
