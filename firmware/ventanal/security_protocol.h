// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
#pragma once
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <string>
#ifdef VENTANAL_HOST_TEST
#include <CommonCrypto/CommonHMAC.h>
#else
#include <mbedtls/md.h>
#endif

inline std::string hexBytes(const unsigned char* p,size_t n) {
    const char* digits="0123456789abcdef"; std::string out;
    for(size_t i=0;i<n;++i) { out+=digits[p[i]>>4]; out+=digits[p[i]&15]; }
    return out;
}
inline bool hex64(const char* s) {
    if(strlen(s)!=64) return false;
    for(unsigned i=0;i<64;++i) if(!((s[i]>='0'&&s[i]<='9')||(s[i]>='a'&&s[i]<='f'))) return false;
    return true;
}
inline std::string decodeDeviceKey(const char* s) {
    if(!hex64(s)) return {};
    std::string key;
    for(unsigned i=0;i<64;i+=2) {
        auto nibble=[](char c) { return c<='9'?c-'0':c-'a'+10; };
        key+=char((nibble(s[i])<<4)|nibble(s[i+1]));
    }
    return key;
}
inline std::string hmacHex(const std::string& key,const std::string& data) {
    unsigned char out[32];
#ifdef VENTANAL_HOST_TEST
    CCHmac(kCCHmacAlgSHA256,key.data(),key.size(),data.data(),data.size(),out);
#else
    if(mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256),
        reinterpret_cast<const unsigned char*>(key.data()),key.size(),
        reinterpret_cast<const unsigned char*>(data.data()),data.size(),out)!=0) return {};
#endif
    return hexBytes(out,32);
}
inline std::string requestProof(const std::string& key,const char* method,const char* path,uint64_t counter) {
    char number[21]; snprintf(number,sizeof number,"%llu",static_cast<unsigned long long>(counter));
    return hmacHex(key,std::string(method)+'\n'+path+'\n'+number);
}
