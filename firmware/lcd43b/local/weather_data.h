// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
//
// Standalone weather data: Open-Meteo forecast and NWS alerts, parsed the way
// the server's ventanal.weather.data does (fetch_weather, fetch_alerts) so the
// board's screens match host mode. Pure; host tested against the server's own
// parse of the same fixtures (tools/standalone_expected.py).
#pragma once

#include <ArduinoJson.h>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>

namespace lcd::local {

struct LocalDate {
    int16_t year = 0;
    uint8_t month = 0, day = 0;
};

struct LocalClock {  // a wall-clock time in the forecast's own zone
    LocalDate date;
    uint8_t hour = 0, minute = 0;
};

struct LocalDay {
    LocalDate date;
    int16_t hi = 0, lo = 0;
    uint8_t code = 0;
    uint8_t precip = 0;  // percent
};

inline constexpr int localForecastDays = 7;

struct LocalWeather {
    float temp = 0;
    int humidity = 0;
    float windSpeed = 0;
    char windDir[3]{};
    int code = 0;
    LocalDay days[localForecastDays];
    int dayCount = 0;
    LocalClock sunrise, sunset;
    bool rain = false;      // rain (>= 40 %) later today
    LocalClock rainOnset;
    int32_t utcOffsetSeconds = 0;
};

struct LocalAlert {
    char event[64]{};
    char headline[200]{};
    char severity[12]{};
    bool hasEnd = false;
    int64_t ends = 0;  // epoch seconds
};

// Python's round() and '{:.0f}': halves go to the even neighbour.
inline int roundHalfEven(double value) {
    const double floor = std::floor(value), rest = value - floor;
    if (rest > 0.5) return int(floor) + 1;
    if (rest < 0.5) return int(floor);
    return int(floor) % 2 == 0 ? int(floor) : int(floor) + 1;
}

inline const char* compass8(float degrees) {
    static const char* const names[] = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};
    const double wrapped = std::fmod(std::fmod(double(degrees), 360.0) + 360.0, 360.0);
    return names[int(std::floor((wrapped + 22.5) / 45.0)) % 8];
}

// Days since 1970-01-01 for a proleptic Gregorian date (H. Hinnant's algorithm).
inline int64_t daysFromCivil(int y, unsigned m, unsigned d) {
    y -= m <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = unsigned(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return int64_t(era) * 146097 + int64_t(doe) - 719468;
}

inline LocalDate civilFromDays(int64_t z) {
    z += 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = unsigned(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    const unsigned d = doy - (153 * mp + 2) / 5 + 1;
    const unsigned m = mp < 10 ? mp + 3 : mp - 9;
    LocalDate out;
    out.year = int16_t(int64_t(yoe) + era * 400 + (m <= 2));
    out.month = uint8_t(m);
    out.day = uint8_t(d);
    return out;
}

inline const char* weekdayName(const LocalDate& date) {
    static const char* const names[] = {"THU", "FRI", "SAT", "SUN", "MON", "TUE", "WED"};  // 1970-01-01: Thu
    const int64_t days = daysFromCivil(date.year, date.month, date.day);
    return names[((days % 7) + 7) % 7];
}

namespace detail {

inline bool digits(const char* s, int n, int& out) {
    out = 0;
    for (int i = 0; i < n; ++i) {
        if (s[i] < '0' || s[i] > '9') return false;
        out = out * 10 + (s[i] - '0');
    }
    return true;
}

// "YYYY-MM-DD" and "YYYY-MM-DDTHH:MM" (Open-Meteo local times).
inline bool parseDate(const char* s, LocalDate& out) {
    int y, m, d;
    if (!s || strlen(s) < 10 || s[4] != '-' || s[7] != '-' || !digits(s, 4, y) || !digits(s + 5, 2, m)
        || !digits(s + 8, 2, d) || m < 1 || m > 12 || d < 1 || d > 31)
        return false;
    out.year = int16_t(y);
    out.month = uint8_t(m);
    out.day = uint8_t(d);
    return true;
}

inline bool parseClock(const char* s, LocalClock& out) {
    int h, mi;
    if (!parseDate(s, out.date) || strlen(s) < 16 || s[10] != 'T' || s[13] != ':' || !digits(s + 11, 2, h)
        || !digits(s + 14, 2, mi) || h > 23 || mi > 59)
        return false;
    out.hour = uint8_t(h);
    out.minute = uint8_t(mi);
    return true;
}

inline int64_t clockMinutes(const LocalClock& c) {
    return daysFromCivil(c.date.year, c.date.month, c.date.day) * 1440 + c.hour * 60 + c.minute;
}

template<size_t N>
inline void copyText(char (&dst)[N], const char* src) {
    const size_t n = src ? strnlen(src, N - 1) : 0;
    memcpy(dst, src ? src : "", n);
    dst[n] = '\0';
}

}  // namespace detail

// "2026-11-19T18:00:00-09:00" or "...Z" -> epoch seconds (NWS times).
inline bool parseIsoInstant(const char* s, int64_t& out) {
    LocalClock c;
    int seconds;
    if (!detail::parseClock(s, c) || strlen(s) < 20 || s[16] != ':' || !detail::digits(s + 17, 2, seconds))
        return false;
    const char* zone = s + 19;
    int offset = 0;
    if (*zone == 'Z') {
        offset = 0;
    } else if ((*zone == '+' || *zone == '-') && strlen(zone) >= 6 && zone[3] == ':') {
        int hh, mm;
        if (!detail::digits(zone + 1, 2, hh) || !detail::digits(zone + 4, 2, mm)) return false;
        offset = (hh * 60 + mm) * 60 * (*zone == '-' ? -1 : 1);
    } else {
        return false;
    }
    out = detail::clockMinutes(c) * 60 + seconds - offset;
    return true;
}

// Open-Meteo forecast with the server's parameters (see
// server/src/ventanal/weather/data.py fetch_weather); `now` decides whether
// today's rain is still ahead.
inline bool parseOpenMeteo(const char* json, time_t now, LocalWeather& out) {
    JsonDocument filter;
    for (const char* key : {"temperature_2m", "relative_humidity_2m", "weather_code", "wind_speed_10m",
                            "wind_direction_10m"})
        filter["current"][key] = true;
    filter["hourly"]["time"] = true;
    filter["hourly"]["precipitation_probability"] = true;
    for (const char* key : {"time", "weather_code", "temperature_2m_max", "temperature_2m_min",
                            "precipitation_probability_max", "sunrise", "sunset"})
        filter["daily"][key] = true;
    filter["utc_offset_seconds"] = true;
    JsonDocument doc;
    if (!json || deserializeJson(doc, json, DeserializationOption::Filter(filter))) return false;
    JsonObjectConst current = doc["current"], daily = doc["daily"], hourly = doc["hourly"];
    if (!current["temperature_2m"].is<float>() || !current["relative_humidity_2m"].is<int>()
        || !current["weather_code"].is<int>() || !current["wind_speed_10m"].is<float>()
        || !current["wind_direction_10m"].is<float>() || !doc["utc_offset_seconds"].is<int>())
        return false;
    out = LocalWeather{};
    out.temp = current["temperature_2m"].as<float>();
    out.humidity = current["relative_humidity_2m"].as<int>();
    out.code = current["weather_code"].as<int>();
    out.windSpeed = current["wind_speed_10m"].as<float>();
    detail::copyText(out.windDir, compass8(current["wind_direction_10m"].as<float>()));
    out.utcOffsetSeconds = doc["utc_offset_seconds"].as<int>();

    JsonArrayConst dates = daily["time"];
    if (dates.size() == 0) return false;
    for (JsonVariantConst value : dates) {
        if (out.dayCount == localForecastDays) break;
        const int i = out.dayCount;
        LocalDay& day = out.days[i];
        if (!detail::parseDate(value.as<const char*>(), day.date) || !daily["temperature_2m_max"][i].is<float>()
            || !daily["temperature_2m_min"][i].is<float>())
            return false;
        day.hi = int16_t(roundHalfEven(daily["temperature_2m_max"][i].as<double>()));
        day.lo = int16_t(roundHalfEven(daily["temperature_2m_min"][i].as<double>()));
        day.code = uint8_t(daily["weather_code"][i] | 0);
        day.precip = uint8_t(daily["precipitation_probability_max"][i] | 0);
        ++out.dayCount;
    }
    if (!detail::parseClock(daily["sunrise"][0], out.sunrise) || !detail::parseClock(daily["sunset"][0], out.sunset))
        return false;

    // The first hour today, strictly after now, with a 40 % or higher chance.
    const LocalDate today = civilFromDays((int64_t(now) + out.utcOffsetSeconds) / 86400);
    JsonArrayConst hours = hourly["time"];
    JsonArrayConst chances = hourly["precipitation_probability"];
    int index = 0;
    for (JsonVariantConst value : hours) {
        JsonVariantConst chance = chances[index++];
        LocalClock hour;
        if (chance.isNull() || chance.as<int>() < 40 || !detail::parseClock(value.as<const char*>(), hour)) continue;
        if (hour.date.year != today.year || hour.date.month != today.month || hour.date.day != today.day) continue;
        if (int64_t(detail::clockMinutes(hour)) * 60 <= (int64_t(now) + out.utcOffsetSeconds)) continue;
        if (!out.rain || detail::clockMinutes(hour) < detail::clockMinutes(out.rainOnset)) {
            out.rain = true;
            out.rainOnset = hour;
        }
    }
    return true;
}

// NWS active alerts for a point, most severe first (stable within a level),
// as many as fit. Returns the count, or -1 when the request failed (keep the
// previous alerts). A 4xx answer means the point has no NWS coverage: none.
inline int parseNwsAlerts(int status, const char* json, LocalAlert* out, int max) {
    if (status >= 400 && status < 500) return 0;
    if (status != 200 || !json) return -1;
    JsonDocument filter;
    for (const char* key : {"event", "headline", "severity", "ends"})
        filter["features"][0]["properties"][key] = true;
    JsonDocument doc;
    if (deserializeJson(doc, json, DeserializationOption::Filter(filter)) || !doc["features"].is<JsonArrayConst>())
        return -1;
    static const char* const order[] = {"Extreme", "Severe", "Moderate", "Minor"};
    int count = 0;
    for (int level = 0; level <= 4 && count < max; ++level) {
        for (JsonVariantConst feature : doc["features"].as<JsonArrayConst>()) {
            if (count == max) break;
            JsonObjectConst p = feature["properties"];
            const char* severity = p["severity"] | "Unknown";
            int rank = 4;
            for (int i = 0; i < 4; ++i)
                if (!strcmp(severity, order[i])) rank = i;
            if (rank != level) continue;
            LocalAlert& a = out[count++];
            a = LocalAlert{};
            detail::copyText(a.event, p["event"] | "");
            detail::copyText(a.headline, p["headline"] | "");
            detail::copyText(a.severity, *severity ? severity : "Unknown");
            a.hasEnd = parseIsoInstant(p["ends"] | "", a.ends);
        }
    }
    return count;
}

}  // namespace lcd::local
