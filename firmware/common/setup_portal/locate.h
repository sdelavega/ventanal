// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
//
// ZIP -> coordinates and place (api.zippopotam.us, the host's geocoder) ->
// IANA zone (Open-Meteo, timezone=auto) -> POSIX TZ (tz_posix.h). The ZIP
// goes only to these two services. Parsing is pure; resolveLocation (in
// locate.cpp) does the two HTTPS requests on the boards.
#pragma once

#include "../tz_posix.h"
#include "location.h"
#include <ArduinoJson.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace portal {

inline void zipUrl(const char* zip, char* out, size_t size) {
    snprintf(out, size, "https://api.zippopotam.us/us/%.5s", zip);  // ZIP+4 -> ZIP
}

inline void timezoneUrl(float latitude, float longitude, char* out, size_t size) {
    snprintf(out, size,
             "https://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f&timezone=auto&forecast_days=1",
             double(latitude), double(longitude));
}

namespace detail {

inline bool parseCoordinate(JsonVariantConst value, float limit, float& out) {
    const char* text = value.as<const char*>();
    if (!text || !*text) return false;
    char* end = nullptr;
    const float v = strtof(text, &end);
    if (*end || !(v >= -limit && v <= limit)) return false;
    out = v;
    return true;
}

// Copies at most size-1 bytes without cutting a UTF-8 sequence in half.
inline void copyUtf8(char* out, size_t size, const char* text) {
    size_t n = strlen(text);
    if (n >= size) {
        n = size - 1;
        while (n && (static_cast<unsigned char>(text[n]) & 0xC0) == 0x80) --n;
    }
    memcpy(out, text, n);
    out[n] = '\0';
}

}  // namespace detail

inline bool parseZippopotam(const char* json, float& latitude, float& longitude, char place[48]) {
    JsonDocument doc;
    if (deserializeJson(doc, json)) return false;
    JsonVariantConst first = doc["places"][0];
    float lat, lon;
    if (first.isNull() || !detail::parseCoordinate(first["latitude"], 90.f, lat)
        || !detail::parseCoordinate(first["longitude"], 180.f, lon))
        return false;
    const char* name = first["place name"] | "";
    const char* state = first["state abbreviation"] | "";
    char text[160];
    snprintf(text, sizeof text, "%s%s%s", name, *name && *state ? ", " : "", state);
    detail::copyUtf8(place, 48, text);
    latitude = lat;
    longitude = lon;
    return true;
}

inline bool parseOpenMeteoTimezone(const char* json, char iana[64]) {
    JsonDocument filter;
    filter["timezone"] = true;
    JsonDocument doc;
    if (deserializeJson(doc, json, DeserializationOption::Filter(filter))) return false;
    const char* zone = doc["timezone"] | "";
    if (!*zone || strlen(zone) >= 64) return false;
    strcpy(iana, zone);
    return true;
}

// Step 1: the geocoder's answer. ok stays false until the time zone is known.
inline LocationResult locationFromZip(int status, const char* body) {
    LocationResult r;
    if (status == 404) {
        r.why = LocationResult::ZipUnknown;
    } else if (status != 200) {
        r.why = LocationResult::Network;
    } else if (!parseZippopotam(body, r.latitude, r.longitude, r.place)) {
        r.why = LocationResult::ZipUnknown;  // 200 with no places: the geocoder doesn't know it
    } else {
        r.why = LocationResult::Ok;
    }
    return r;
}

// Step 2: Open-Meteo's zone name, mapped to the stored POSIX rule.
inline void finishLocation(LocationResult& r, int status, const char* body) {
    if (r.why != LocationResult::Ok) return;
    char iana[64];
    if (status != 200 || !parseOpenMeteoTimezone(body, iana)) {
        r.why = LocationResult::Network;
        return;
    }
    const char* posix = posixForIana(iana);
    if (!posix || strlen(posix) >= sizeof r.posixTz) {
        r.why = LocationResult::TzUnknown;
        return;
    }
    strcpy(r.posixTz, posix);
    r.ok = true;
}

#ifndef VENTANAL_HOST_TEST
LocationResult resolveLocation(const char* zip);
#endif

}  // namespace portal
