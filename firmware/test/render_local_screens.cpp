// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
//
// Host build of the board's standalone renderers: draws the fixture cases
// (the same ones tools/standalone_render.py asks the server for) and writes
// them as PPM images for tools/standalone_compare.py.
#include "../lcd43b/local/render_weather.h"
#include <cassert>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace lcd::local;

static std::string fixture(const std::string& name) {
    std::ifstream in("fixtures/standalone/" + name);
    if (!in) {
        fprintf(stderr, "missing fixture %s (run from firmware/test)\n", name.c_str());
        exit(2);
    }
    std::stringstream text;
    text << in.rdbuf();
    return text.str();
}

static time_t fixtureNow(const char* expectedName) {
    JsonDocument doc;
    if (deserializeJson(doc, fixture(expectedName))) exit(2);
    return time_t(doc["now_epoch"].as<long long>());
}

static void writePpm(const std::string& path, const std::vector<uint16_t>& px) {
    std::ofstream out(path, std::ios::binary);
    out << "P6\n" << FRAME_WIDTH << " " << FRAME_HEIGHT << "\n255\n";
    for (uint16_t c : px) {
        const unsigned r = (c >> 11) & 0x1f, g = (c >> 5) & 0x3f, b = c & 0x1f;
        const char rgb[3] = {char((r << 3) | (r >> 2)), char((g << 2) | (g >> 4)), char((b << 3) | (b >> 2))};
        out.write(rgb, 3);
    }
}

struct WeatherCase {
    const char* name;
    const char* forecast;
    const char* expected;  // supplies the fixture's clock
    bool metric, clock24h;
    const char* alerts;    // nullptr: none
};

int main(int argc, char** argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s OUT_DIR\n", argv[0]);
        return 2;
    }
    const WeatherCase cases[] = {  // tools/standalone_render.py WEATHER_CASES
        {"weather-imperial-12h", "open-meteo-90210-imperial.json", "expected-weather-imperial.json", false, false,
         "nws-90210.json"},
        {"weather-metric-24h", "open-meteo-90210-metric.json", "expected-weather-metric.json", true, true,
         "nws-alerts-ak.json"},
        {"weather-early-quiet", "open-meteo-90210-imperial.json", "expected-weather-early.json", false, false, nullptr},
    };
    std::vector<uint16_t> px(size_t(FRAME_WIDTH) * FRAME_HEIGHT);
    for (const WeatherCase& c : cases) {
        const time_t now = fixtureNow(c.expected);
        LocalWeather w;
        if (!parseOpenMeteo(fixture(c.forecast).c_str(), now, w)) return 1;
        LocalAlert alerts[4];
        const int count = c.alerts ? parseNwsAlerts(200, fixture(c.alerts).c_str(), alerts, 4) : 0;
        WeatherScreen screen;
        screen.metric = c.metric;
        screen.clock24h = c.clock24h;
        screen.place = "Beverly Hills, CA";
        screen.latitude = 34.0901f;  // as stored in the installation record
        screen.longitude = -118.4065f;
        renderWeather(Frame{px.data()}, w, alerts, count < 0 ? 0 : count, screen, now);
        writePpm(std::string(argv[1]) + "/" + c.name + ".ppm", px);
    }
    return 0;
}
