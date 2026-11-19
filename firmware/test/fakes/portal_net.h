// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
// Fake network hooks for the setup portal's host tests.
#pragma once

#include "../../common/setup_portal/portal.h"
#include <cstring>

namespace fakenet {

inline int joins, locates, saves, feeds;
inline bool joinResult, saveResult;
inline char lastSsid[33], lastPassword[65], lastZip[11];
inline uint32_t lastTimeoutMs;
inline portal::LocationResult location;
inline portal::PortalFields savedFields;
inline portal::LocationResult savedLocation;

inline void reset() {
    joins = locates = saves = feeds = 0;
    joinResult = saveResult = true;
    lastSsid[0] = lastPassword[0] = lastZip[0] = '\0';
    lastTimeoutMs = 0;
    location = portal::LocationResult{};
    location.ok = true;
    location.why = portal::LocationResult::Ok;
    location.latitude = 40.0f;
    location.longitude = -75.0f;
    strcpy(location.place, "Sample, PA");
    strcpy(location.posixTz, "EST5EDT,M3.2.0,M11.1.0");
    savedFields = portal::PortalFields{};
    savedLocation = portal::LocationResult{};
}

inline portal::PortalHooks hooks() {
    portal::PortalHooks h;
    h.join = [](const char* ssid, const char* password, uint32_t timeoutMs) {
        ++joins;
        strcpy(lastSsid, ssid);
        strcpy(lastPassword, password);
        lastTimeoutMs = timeoutMs;
        return joinResult;
    };
    h.locate = [](const char* zip) {
        ++locates;
        strcpy(lastZip, zip);
        return location;
    };
    h.save = [](const portal::PortalFields& f, const portal::LocationResult& l) {
        ++saves;
        savedFields = f;
        savedLocation = l;
        return saveResult;
    };
    h.feed = [] { ++feeds; };
    return h;
}

}  // namespace fakenet
