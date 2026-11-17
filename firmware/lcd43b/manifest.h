// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
#pragma once

// Device manifest parsing for the lcd43b profile. The server publishes
// <output>/devices/lcd43b/manifest.json:
//   {"version": 1,
//    "screens": {"art": {...}, "weather": {...}, "solar": {...}},
//    "spares": [{...}, ...],        // optional, at most three
//    "quiet": {"start": "23:00", "end": "06:00"}}
// Every frame entry carries id (16 lowercase hex), url (under /devices/),
// size (exactly one VNR1 frame) and a full lowercase-hex SHA-256.

#include <ArduinoJson.h>
#include <cstdint>
#include <cstring>

#include "frame.h"

namespace lcd {

constexpr uint8_t SPARE_SLOTS = 3;

struct FrameInfo {
    char id[17] = {};
    char url[128] = {};
    char sha256[65] = {};
    bool present = false;
};

struct DeviceManifest {
    FrameInfo screens[3];       // art, weather, solar
    FrameInfo spares[SPARE_SLOTS];
    uint8_t spareCount = 0;
    int quietStart = -1;         // minutes of local day; <0 disables quiet
    int quietEnd = -1;
    bool quietValid() const { return quietStart >= 0 && quietEnd >= 0; }
};

// "23:00" -> 1380; anything else -> -1.
inline int parseClock(const char* text) {
    if (!text || strlen(text) != 5 || text[2] != ':') return -1;
    for (int i : {0, 1, 3, 4})
        if (text[i] < '0' || text[i] > '9') return -1;
    const int hour = (text[0] - '0') * 10 + (text[1] - '0');
    const int minute = (text[3] - '0') * 10 + (text[4] - '0');
    if (hour > 23 || minute > 59) return -1;
    return hour * 60 + minute;
}

inline bool lowerHex(const char* text, size_t length) {
    for (size_t i = 0; i < length; ++i) {
        const char c = text[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }
    return true;
}

inline bool parseFrameInfo(JsonVariantConst value, FrameInfo& out) {
    const char* id = value["id"] | "";
    const char* url = value["url"] | "";
    const char* sha = value["sha256"] | "";
    const size_t size = value["size"] | 0u;
    if (strlen(id) != 16 || strlen(sha) != 64 || size != FRAME_BYTES) return false;
    if (strlen(url) >= sizeof out.url) return false;
    if (!lowerHex(id, 16) || !lowerHex(sha, 64)) return false;
    if (strncmp(url, "/devices/", 9) != 0 || strstr(url, "..")) return false;
    strcpy(out.id, id);
    strcpy(out.url, url);
    strcpy(out.sha256, sha);
    out.present = true;
    return true;
}

// Validate and copy a device manifest document; false leaves out untouched.
inline bool parseDeviceManifest(const char* body, DeviceManifest& out) {
    JsonDocument doc;
    if (deserializeJson(doc, body) || (doc["version"] | 0) != 1) return false;
    DeviceManifest parsed;
    if (!parseFrameInfo(doc["screens"]["art"], parsed.screens[0])) return false;
    if (!parseFrameInfo(doc["screens"]["weather"], parsed.screens[1])) return false;
    if (!doc["screens"]["solar"].isNull()
        && !parseFrameInfo(doc["screens"]["solar"], parsed.screens[2])) return false;
    if (!doc["spares"].isNull()) {
        if (!doc["spares"].is<JsonArrayConst>() || doc["spares"].size() > SPARE_SLOTS)
            return false;
        for (JsonVariantConst spare : doc["spares"].as<JsonArrayConst>()) {
            if (!parseFrameInfo(spare, parsed.spares[parsed.spareCount])) return false;
            ++parsed.spareCount;
        }
    }
    if (!doc["quiet"].isNull()) {
        parsed.quietStart = parseClock(doc["quiet"]["start"] | "");
        parsed.quietEnd = parseClock(doc["quiet"]["end"] | "");
        if (parsed.quietStart < 0 || parsed.quietEnd < 0) return false;
    }
    out = parsed;
    return true;
}

// Quiet hours: equal endpoints disable quiet; a wrapped window (start after
// end) spans midnight.
inline bool inQuietMinutes(int minuteOfDay, int start, int end) {
    if (start < 0 || start == end) return false;
    return start < end ? (minuteOfDay >= start && minuteOfDay < end)
                       : (minuteOfDay >= start || minuteOfDay < end);
}

// The earliest wall-clock year the firmware treats as trustworthy: before
// the first NTP sync the ESP32 reports 1970, so quiet hours would darken
// the panel at nonsense times. Until the clock is valid the backlight
// stays on — defined, safe behaviour for a USB-powered display (pinned by
// test_lcd43b_manifest and test_lcd43b_persist).
constexpr int MIN_VALID_YEAR = 2023;

// True when the (possibly persisted) quiet window darkens the panel right
// now: the window must be valid and the clock trustworthy.
inline bool quietApplies(int year, int minuteOfDay, const DeviceManifest& m) {
    if (!m.quietValid() || year < MIN_VALID_YEAR) return false;
    return inQuietMinutes(minuteOfDay, m.quietStart, m.quietEnd);
}

}  // namespace lcd