// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
#pragma once
#include "Preferences.h"
using nvs_handle_t = int;
using esp_err_t = int;
using nvs_type_t = int;
constexpr int ESP_OK=0;
constexpr int NVS_READWRITE=1;
constexpr int NVS_READONLY=0, ESP_ERR_NVS_NOT_FOUND=1, ESP_ERR_NO_MEM=2;
constexpr int ESP_ERR_NVS_INVALID_LENGTH=3;
inline std::map<std::string,std::vector<uint8_t>> lcdSecurityNvs;
inline bool lcdSecurityNamespaceExists=false, failLcdSecurityWrite=false;
inline unsigned lcdSecurityWrites=0;
inline bool failRawStateRead=false, failQuarantineWrite=false, failKeyLookup=false, failNvsOpen=false;
inline int nvs_open(const char* space,int mode,nvs_handle_t* handle) {
    if(failNvsOpen) return ESP_ERR_NO_MEM;
    if (strcmp(space, "lcd-sec") == 0) {
        if (mode == NVS_READONLY && !lcdSecurityNamespaceExists) return ESP_ERR_NVS_NOT_FOUND;
        if (mode == NVS_READWRITE) lcdSecurityNamespaceExists = true;
        *handle = 2;
        return ESP_OK;
    }
    if(mode==NVS_READONLY && !preferencesNamespaceExists) return ESP_ERR_NVS_NOT_FOUND;
    if(mode==NVS_READWRITE) preferencesNamespaceExists=true;
    *handle=1;
    return ESP_OK;
}
inline int nvs_find_key(nvs_handle_t,const char* key,nvs_type_t*) {
    return failKeyLookup ? -1 : preferences.count(key) ? ESP_OK : ESP_ERR_NVS_NOT_FOUND;
}
inline int nvs_get_blob(nvs_handle_t handle,const char* key,void* bytes,size_t* length) {
    auto& values = handle == 2 ? lcdSecurityNvs : preferences;
    if(failRawStateRead) return -1;
    if(!values.count(key)) return ESP_ERR_NVS_NOT_FOUND;
    if(*length<values[key].size()) return ESP_ERR_NVS_INVALID_LENGTH;
    *length=values[key].size();
    if(*length) memcpy(bytes,values[key].data(),*length);
    return ESP_OK;
}
inline int nvs_set_blob(nvs_handle_t handle,const char* key,const void* bytes,size_t length) {
    if(failQuarantineWrite || (handle == 2 && failLcdSecurityWrite)) return -1;
    const auto* raw=static_cast<const uint8_t*>(bytes);
    auto& values = handle == 2 ? lcdSecurityNvs : preferences;
    values[key]={raw,raw+length};
    if (handle == 2) ++lcdSecurityWrites;
    return ESP_OK;
}
inline int nvs_commit(nvs_handle_t) { return 0; }
inline void nvs_close(nvs_handle_t) {}
