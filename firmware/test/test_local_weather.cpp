// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
#include "../lcd43b/local/weather_data.h"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

using namespace lcd::local;

static std::string fixture(const std::string& name) {
    std::ifstream in("fixtures/standalone/" + name);
    assert(in && "run from firmware/test");
    std::stringstream text;
    text << in.rdbuf();
    return text.str();
}

static std::string clockText(const LocalClock& c) {
    char out[16];
    snprintf(out, sizeof out, "%02d:%02d", c.hour, c.minute);
    return out;
}

static std::string stampText(const LocalClock& c) {
    char out[40];
    snprintf(out, sizeof out, "%04d-%02d-%02dT%02d:%02d", c.date.year, c.date.month, c.date.day, c.hour, c.minute);
    return out;
}

// The server's parse of the same fixture (tools/standalone_expected.py).
static void matches_server(const char* expectedName) {
    JsonDocument expected;
    assert(!deserializeJson(expected, fixture(expectedName)));
    LocalWeather w{};
    const std::string body = fixture(expected["fixture"].as<const char*>());
    assert(parseOpenMeteo(body.c_str(), time_t(expected["now_epoch"].as<long long>()), w));
    JsonObjectConst current = expected["current"];
    assert(std::fabs(w.temp - current["temp"].as<float>()) < 1e-4f);
    assert(w.humidity == current["humidity"].as<int>());
    assert(std::fabs(w.windSpeed - current["wind_speed"].as<float>()) < 1e-4f);
    assert(!strcmp(w.windDir, current["wind_dir"].as<const char*>()) && w.code == current["code"].as<int>());
    JsonArrayConst days = expected["days"];
    assert(w.dayCount == int(days.size()));
    for (int i = 0; i < w.dayCount; ++i) {
        char date[32];
        snprintf(date, sizeof date, "%04d-%02d-%02d", w.days[i].date.year, w.days[i].date.month, w.days[i].date.day);
        assert(date == std::string(days[i]["date"].as<const char*>()));
        assert(!strcmp(weekdayName(w.days[i].date), days[i]["weekday"].as<const char*>()));
        assert(w.days[i].hi == days[i]["hi"].as<int>() && w.days[i].lo == days[i]["lo"].as<int>());
        assert(w.days[i].code == days[i]["code"].as<int>() && w.days[i].precip == days[i]["precip"].as<int>());
    }
    assert(clockText(w.sunrise) == expected["sunrise"].as<const char*>());
    assert(clockText(w.sunset) == expected["sunset"].as<const char*>());
    if (expected["rain_onset"].isNull()) assert(!w.rain);
    else assert(w.rain && stampText(w.rainOnset) == expected["rain_onset"].as<const char*>());
    JsonArrayConst hours = expected["hours"];
    assert(hours.size() == 12 && w.hourCount == 12);
    for (int i = 0; i < w.hourCount; ++i) {
        assert(stampText(w.hours[i].time) == hours[i]["time"].as<const char*>());
        assert(std::fabs(w.hours[i].temp - hours[i]["temp"].as<float>()) < 1e-4f);
        assert(w.hours[i].precip == hours[i]["precip"].as<int>());
    }
}

static void open_meteo_matches_server() {
    matches_server("expected-weather-imperial.json");
    matches_server("expected-weather-metric.json");
    matches_server("expected-weather-early.json");  // today's rain still ahead
}

static void open_meteo_rejects_bad_bodies() {
    LocalWeather w{};
    assert(!parseOpenMeteo("", 0, w));
    assert(!parseOpenMeteo("<html>busy</html>", 0, w));
    assert(!parseOpenMeteo("{\"current\":{}}", 0, w));
    assert(!parseOpenMeteo("{\"error\":true,\"reason\":\"Latitude must be in range\"}", 0, w));
}

static void rounding_matches_python() {
    // Python's round() and '{:.0f}' round halves to even.
    assert(roundHalfEven(2.5) == 2 && roundHalfEven(3.5) == 4 && roundHalfEven(-2.5) == -2);
    assert(roundHalfEven(62.4) == 62 && roundHalfEven(62.6) == 63 && roundHalfEven(-0.4) == 0);
}

static void compass8_matches_server() {
    // ('N','NE',...)[int((deg % 360 + 22.5) // 45) % 8]
    const struct { float deg; const char* dir; } cases[] = {
        {0, "N"}, {22.4f, "N"}, {22.5f, "NE"}, {90, "E"}, {180, "S"}, {225, "SW"},
        {337.4f, "NW"}, {337.5f, "N"}, {359, "N"}, {360, "N"}, {-10, "N"},
    };
    for (const auto& c : cases) assert(!strcmp(compass8(c.deg), c.dir));
}

static void nws_alerts_parsed(const char* fixtureName, const char* expectedName) {
    JsonDocument expected;
    assert(!deserializeJson(expected, fixture(expectedName)));
    LocalAlert alerts[4];
    const std::string body = fixture(fixtureName);
    const int n = parseNwsAlerts(200, body.c_str(), alerts, 4);
    JsonArrayConst want = expected["alerts"];
    assert(n == int(want.size()));
    for (int i = 0; i < n; ++i) {
        assert(!strcmp(alerts[i].event, want[i]["event"].as<const char*>()));
        assert(!strcmp(alerts[i].severity, want[i]["severity"].as<const char*>()));
        assert(!strncmp(alerts[i].headline, want[i]["headline"].as<const char*>(), sizeof alerts[i].headline - 1));
        if (want[i]["ends_epoch"].isNull()) assert(!alerts[i].hasEnd);
        else assert(alerts[i].hasEnd && alerts[i].ends == want[i]["ends_epoch"].as<long long>());
    }
}

static void nws_status_rules() {
    LocalAlert alerts[2];
    assert(parseNwsAlerts(404, "{}", alerts, 2) == 0);  // outside the NWS area: no alerts, not a failure
    assert(parseNwsAlerts(400, "{\"title\":\"Bad Request\"}", alerts, 2) == 0);
    assert(parseNwsAlerts(503, "", alerts, 2) == -1);  // keep the previous alerts
    assert(parseNwsAlerts(-1, "", alerts, 2) == -1);
    assert(parseNwsAlerts(200, "<html>", alerts, 2) == -1);
    assert(parseNwsAlerts(200, "{\"features\":[]}", alerts, 2) == 0);
    // More alerts than room: the most severe are kept, in order.
    const std::string body = fixture("nws-alerts-ak.json");
    assert(parseNwsAlerts(200, body.c_str(), alerts, 2) == 2);
    assert(!strcmp(alerts[0].severity, "Severe") && !strcmp(alerts[1].severity, "Moderate"));
}

static void iso_times_with_offsets() {
    int64_t t = 0;
    assert(parseIsoInstant("2026-11-19T18:00:00-09:00", t) && t == 1795143600);
    assert(parseIsoInstant("1970-01-01T00:00:00+00:00", t) && t == 0);
    assert(parseIsoInstant("2026-03-08T02:30:00Z", t));
    assert(!parseIsoInstant("2026-11-19", t) && !parseIsoInstant("", t));
}

int main() {
    open_meteo_matches_server();
    open_meteo_rejects_bad_bodies();
    rounding_matches_python();
    compass8_matches_server();
    nws_alerts_parsed("nws-alerts-ak.json", "expected-alerts-ak.json");
    nws_alerts_parsed("nws-90210.json", "expected-alerts-90210.json");
    nws_status_rules();
    iso_times_with_offsets();
    puts("local weather: Open-Meteo and NWS parsing match the server, status rules, rounding passed");
}
