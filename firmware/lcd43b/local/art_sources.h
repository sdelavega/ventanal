// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
//
// Standalone art: the museum searches the server uses (art/sources/aic.py,
// cleveland.py, met.py) with the same rights filters and credit fields
// (manifest.clean_credit_text). Images are requested at screen size instead
// of the host's print size: AIC IIIF just wide enough to cover 800x480,
// Cleveland's 900 px "web" image, the Met's primaryImageSmall. Pure; host
// tested against the server's own parse of the same fixtures.
#pragma once

#include <ArduinoJson.h>
#include <cctype>
#include <cstdlib>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace lcd::local {

struct ArtCandidate {
    char source[12]{};     // "aic", "cleveland", "met"
    char objectId[24]{};
    char title[200]{};
    char artist[121]{};    // the server keeps 120 characters of artist, date and museum
    char date[121]{};
    char museum[121]{};
    char imageUrl[256]{};
    uint16_t width = 0, height = 0;  // of the requested image, when the source says
};

inline constexpr int ART_WIDTH = 800, ART_HEIGHT = 480;

namespace art_detail {

// clean_credit_text(): drop C0/C1 controls (and invalid UTF-8), keep at most
// `limit` characters, never cutting a character in half.
template<size_t N>
inline void cleanCopy(char (&dst)[N], const char* src, size_t limit = N) {
    size_t out = 0, chars = 0;
    const auto* p = reinterpret_cast<const unsigned char*>(src ? src : "");
    while (*p && chars < limit) {
        const unsigned char c = *p;
        const size_t len = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xe ? 3 : (c >> 3) == 0x1e ? 4 : 0;
        if (!len) {
            ++p;
            continue;
        }
        bool complete = true;
        for (size_t i = 1; i < len; ++i) complete = complete && (p[i] & 0xc0) == 0x80;
        if (!complete) {
            ++p;
            continue;
        }
        const bool c0 = len == 1 && (c < 0x20 || c == 0x7f);
        const bool c1 = len == 2 && c == 0xc2 && p[1] >= 0x80 && p[1] <= 0x9f;
        if (!c0 && !c1) {
            if (out + len >= N) break;
            memcpy(dst + out, p, len);
            out += len;
            ++chars;
        }
        p += len;
    }
    dst[out] = '\0';
}

inline void urlEncode(const char* text, char* out, size_t size) {
    static const char hex[] = "0123456789ABCDEF";
    size_t n = 0;
    for (const auto* p = reinterpret_cast<const unsigned char*>(text); *p && n + 4 < size; ++p) {
        if (isalnum(*p) || *p == '-' || *p == '_' || *p == '.' || *p == '~') {
            out[n++] = char(*p);
        } else {
            out[n++] = '%';
            out[n++] = hex[*p >> 4];
            out[n++] = hex[*p & 15];
        }
    }
    out[n] = '\0';
}

inline uint16_t dimension(JsonVariantConst v) {
    if (v.is<int>()) return uint16_t(v.as<int>() > 0 && v.as<int>() < 65536 ? v.as<int>() : 0);
    const char* s = v.as<const char*>();
    const long n = s ? strtol(s, nullptr, 10) : 0;
    return uint16_t(n > 0 && n < 65536 ? n : 0);
}

}  // namespace art_detail

// The AIC IIIF width that covers 800x480 (never wider than the original:
// IIIF refuses to upscale).
inline int aicCoverWidth(int originalWidth, int originalHeight) {
    if (originalWidth <= 0 || originalHeight <= 0) return ART_WIDTH;
    const long long byHeight = (static_cast<long long>(ART_HEIGHT) * originalWidth + originalHeight - 1) / originalHeight;
    const long long wanted = byHeight > ART_WIDTH ? byHeight : ART_WIDTH;
    return int(wanted < originalWidth ? wanted : originalWidth);
}

inline void aicSearchUrl(const char* term, int limit, char* out, size_t size) {
    char q[128];
    art_detail::urlEncode(term, q, sizeof q);
    snprintf(out, size,
             "https://api.artic.edu/api/v1/artworks/search?q=%s&query%%5Bterm%%5D%%5Bis_public_domain%%5D=true"
             "&limit=%d&fields=id,title,artist_title,date_display,image_id,thumbnail,is_public_domain",
             q, limit);
}

inline void clevelandSearchUrl(const char* term, int limit, char* out, size_t size) {
    char q[128];
    art_detail::urlEncode(term, q, sizeof q);
    snprintf(out, size,
             "https://openaccess-api.clevelandart.org/api/artworks/?q=%s&cc0=1&has_image=1&limit=%d"
             "&fields=id,title,creators,creation_date,images,share_license_status",
             q, limit);
}

inline void metSearchUrl(const char* term, int limit, char* out, size_t size) {
    char q[128];
    art_detail::urlEncode(term, q, sizeof q);
    snprintf(out, size,
             "https://collectionapi.metmuseum.org/public/collection/v1.1/search?q=%s&hasImages=true&offset=0&limit=%d",
             q, limit);
}

inline void metObjectUrl(uint32_t id, char* out, size_t size) {
    snprintf(out, size, "https://collectionapi.metmuseum.org/public/collection/v1/objects/%lu",
             static_cast<unsigned long>(id));
}

// Returns the candidates found, or -1 when the body is not the expected JSON.
inline int parseAicSearch(const char* json, ArtCandidate* out, int max) {
    JsonDocument filter;
    for (const char* key : {"id", "title", "artist_title", "date_display", "image_id", "thumbnail", "is_public_domain"})
        filter["data"][0][key] = true;
    JsonDocument doc;
    if (!json || deserializeJson(doc, json, DeserializationOption::Filter(filter)) || !doc["data"].is<JsonArrayConst>())
        return -1;
    int n = 0;
    for (JsonObjectConst obj : doc["data"].as<JsonArrayConst>()) {
        if (n == max) break;
        const char* image = obj["image_id"] | "";
        if (!*image || obj["is_public_domain"] != true) continue;  // the returned flag, as well as the query
        ArtCandidate& c = out[n++];
        c = ArtCandidate{};
        strcpy(c.source, "aic");
        snprintf(c.objectId, sizeof c.objectId, "%lld", obj["id"].as<long long>());
        art_detail::cleanCopy(c.title, obj["title"] | "");
        art_detail::cleanCopy(c.artist, obj["artist_title"] | "", 120);
        art_detail::cleanCopy(c.date, obj["date_display"] | "", 120);
        art_detail::cleanCopy(c.museum, "Art Institute of Chicago", 120);
        const int w = art_detail::dimension(obj["thumbnail"]["width"]);
        const int h = art_detail::dimension(obj["thumbnail"]["height"]);
        const int width = aicCoverWidth(w, h);
        c.width = uint16_t(width);
        c.height = uint16_t(w > 0 && h > 0 ? (static_cast<long long>(h) * width + w / 2) / w : 0);
        snprintf(c.imageUrl, sizeof c.imageUrl, "https://www.artic.edu/iiif/2/%s/full/%d,/0/default.jpg", image, width);
    }
    return n;
}

inline int parseClevelandSearch(const char* json, ArtCandidate* out, int max) {
    JsonDocument filter;
    filter["data"][0]["id"] = true;
    filter["data"][0]["title"] = true;
    filter["data"][0]["creation_date"] = true;
    filter["data"][0]["share_license_status"] = true;
    filter["data"][0]["creators"][0]["description"] = true;
    filter["data"][0]["images"]["web"] = true;
    JsonDocument doc;
    if (!json || deserializeJson(doc, json, DeserializationOption::Filter(filter)) || !doc["data"].is<JsonArrayConst>())
        return -1;
    int n = 0;
    for (JsonObjectConst obj : doc["data"].as<JsonArrayConst>()) {
        if (n == max) break;
        JsonObjectConst web = obj["images"]["web"];
        const char* url = web["url"] | "";
        if (!*url || strcmp(obj["share_license_status"] | "", "CC0") || strlen(url) >= sizeof(ArtCandidate::imageUrl))
            continue;
        ArtCandidate& c = out[n++];
        c = ArtCandidate{};
        strcpy(c.source, "cleveland");
        snprintf(c.objectId, sizeof c.objectId, "%lld", obj["id"].as<long long>());
        art_detail::cleanCopy(c.title, obj["title"] | "");
        art_detail::cleanCopy(c.artist, obj["creators"][0]["description"] | "", 120);
        art_detail::cleanCopy(c.date, obj["creation_date"] | "", 120);
        art_detail::cleanCopy(c.museum, "Cleveland Museum of Art", 120);
        strcpy(c.imageUrl, url);
        c.width = art_detail::dimension(web["width"]);
        c.height = art_detail::dimension(web["height"]);
    }
    return n;
}

inline int parseMetSearchIds(const char* json, uint32_t* ids, int max) {
    JsonDocument doc;
    if (!json || deserializeJson(doc, json)) return -1;
    int n = 0;
    for (JsonVariantConst id : doc["objectIDs"].as<JsonArrayConst>()) {
        if (n == max) break;
        if (id.is<unsigned long>()) ids[n++] = uint32_t(id.as<unsigned long>());
    }
    return n;
}

// One object record; false unless it is a public-domain painting, drawing or
// print with an image (the server's MetSource filter).
inline bool parseMetObject(const char* json, ArtCandidate& c) {
    JsonDocument filter;
    for (const char* key : {"objectID", "isPublicDomain", "primaryImage", "primaryImageSmall", "classification",
                            "title", "artistDisplayName", "objectDate"})
        filter[key] = true;
    JsonDocument doc;
    if (!json || deserializeJson(doc, json, DeserializationOption::Filter(filter))) return false;
    char classification[64];
    art_detail::cleanCopy(classification, doc["classification"] | "");
    for (char* p = classification; *p; ++p) *p = char(tolower(static_cast<unsigned char>(*p)));
    const char* small = doc["primaryImageSmall"] | "";
    const char* full = doc["primaryImage"] | "";
    if (doc["isPublicDomain"] != true || !*full || !*small || strlen(small) >= sizeof c.imageUrl
        || !(strstr(classification, "painting") || strstr(classification, "drawing") || strstr(classification, "print")))
        return false;
    c = ArtCandidate{};
    strcpy(c.source, "met");
    snprintf(c.objectId, sizeof c.objectId, "%lld", doc["objectID"].as<long long>());
    art_detail::cleanCopy(c.title, doc["title"] | "");
    art_detail::cleanCopy(c.artist, doc["artistDisplayName"] | "", 120);
    art_detail::cleanCopy(c.date, doc["objectDate"] | "", 120);
    art_detail::cleanCopy(c.museum, "The Metropolitan Museum of Art", 120);
    strcpy(c.imageUrl, small);
    return true;
}

}  // namespace lcd::local
