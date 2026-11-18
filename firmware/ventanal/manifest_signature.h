// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
#pragma once
#include "security_protocol.h"
#include <ArduinoJson.h>
#include <algorithm>
#include <utility>

// Match Python json.dumps(..., ensure_ascii=False), including embedded NULs.
inline void appendCanonicalString(std::string& out, const char* data, size_t size) {
    constexpr char hex[] = "0123456789abcdef";
    out += '"';
    for (size_t i = 0; i < size; ++i) {
        const unsigned char c = data[i];
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    out += "\\u00";
                    out += hex[c >> 4];
                    out += hex[c & 0x0f];
                } else {
                    out += data[i];
                }
        }
    }
    out += '"';
}

// The manifest schema uses integer numbers and UTF-8 strings. Sort object keys
// recursively and retain array order.
inline std::string canonicalJson(JsonVariantConst value) {
    std::string out;
    if(value.is<JsonObjectConst>()) {
        std::vector<std::pair<std::string, JsonVariantConst>> fields;
        for (JsonPairConst pair : value.as<JsonObjectConst>()) {
            const JsonString key = pair.key();
            fields.emplace_back(std::string(key.c_str(), key.size()), pair.value());
        }
        std::sort(fields.begin(), fields.end(), [](const auto& a, const auto& b) {
            return a.first < b.first;
        });
        out="{";
        for (const auto& field : fields) {
            if(out.size()>1) out+=',';
            appendCanonicalString(out, field.first.data(), field.first.size());
            out+=':'; out+=canonicalJson(field.second);
        }
        out+='}';
    } else if(value.is<JsonArrayConst>()) {
        out="[";
        for(JsonVariantConst entry:value.as<JsonArrayConst>()) {
            if(out.size()>1) out+=',';
            out+=canonicalJson(entry);
        }
        out+=']';
    } else if (value.is<JsonString>()) {
        const JsonString str = value.as<JsonString>();
        appendCanonicalString(out, str.c_str(), str.size());
    } else serializeJson(value,out);
    return out;
}
inline bool verifyManifestSignature(JsonDocument& doc, const std::string& key) {
    if (key.size() != 32) return false;
    const std::string signature=doc["sig"] | "";
    if(!hex64(signature.c_str())) return false;
    doc.remove("sig");
    const auto expected=hmacHex(key,canonicalJson(doc.as<JsonVariantConst>()));
    if(expected.size()!=64) return false;
    unsigned diff=0;
    for(unsigned i=0;i<64;++i) diff|=signature[i]^expected[i];
    return diff == 0;
}
