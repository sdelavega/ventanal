// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
//
// Host build of the board's standalone renderers: draws the fixture cases
// (the same ones tools/standalone_render.py asks the server for) and writes
// them as PPM images for tools/standalone_compare.py.
#include "../lcd43b/local/art_compose.h"
#include "../lcd43b/local/render_solar.h"
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

struct Image {
    int width = 0, height = 0;
    std::vector<uint8_t> rgb;
};

static Image readPpm(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    std::string magic;
    Image im;
    int maxValue = 0;
    in >> magic >> im.width >> im.height >> maxValue;
    in.get();
    if (magic != "P6" || maxValue != 255) exit(2);
    im.rgb.resize(size_t(im.width) * im.height * 3);
    in.read(reinterpret_cast<char*>(im.rgb.data()), std::streamsize(im.rgb.size()));
    return im;
}

// Feeds 16x16 blocks in raster order, like a JPEG decoder's MCUs.
static bool feedBlocks(const Image& im, ArtResampler& r) {
    std::vector<uint8_t> block(16 * 16 * 3);
    for (int by = 0; by < im.height; by += 16)
        for (int bx = 0; bx < im.width; bx += 16) {
            const int w = std::min(16, im.width - bx), h = std::min(16, im.height - by);
            for (int y = 0; y < h; ++y)
                memcpy(&block[size_t(y) * w * 3], &im.rgb[(size_t(by + y) * im.width + bx) * 3], size_t(w) * 3);
            r.block(bx, by, w, h, block.data());
        }
    return true;
}

static void renderArtCase(const std::string& dir, const char* name, std::vector<uint16_t>& px) {
    const Image im = readPpm(dir + "/" + name + "-input.ppm");
    std::ifstream in(dir + "/" + name + "-credit.json");
    std::stringstream text;
    text << in.rdbuf();
    JsonDocument doc;
    if (deserializeJson(doc, text.str())) exit(2);
    ArtCredit credit{doc["title"] | "", doc["artist"] | "", doc["date"] | "", doc["museum"] | ""};
    std::fill(px.begin(), px.end(), uint16_t(0));
    if (!composeArt(Frame{px.data()}, im.width, im.height, [&](ArtResampler& r) { return feedBlocks(im, r); }, credit))
        exit(1);
    writePpm(dir + "/" + name + ".ppm", px);
}

// tools/standalone_render.py SOLAR_CASES: the captured SWPC products, with
// the case's alerts in place of the capture's own when it names some.
static void renderSolarCase(const std::string& dir, const char* name, const Image& sun, std::vector<uint16_t>& px) {
    LocalSolar s{};
    if (!parseDailyIndices(fixture("swpc-daily.txt").c_str(), s.sfi, s.ssn) || !parseKp(fixture("swpc-kp.json").c_str(), s)
        || !parseXray(fixture("swpc-xray.json").c_str(), s.xray) || !parseScales(fixture("swpc-scales.json").c_str(), s))
        exit(1);
    parseForecastKp(fixture("swpc-forecast.txt").c_str(), s.kpMax);
    const int64_t now = fixtureNow("expected-solar.json");
    if (!parseActiveAlerts(fixture("swpc-alerts.json").c_str(), now, s.alerts)) exit(1);
    std::ifstream in(dir + "/" + name + "-alerts.json");
    std::stringstream text;
    text << in.rdbuf();
    JsonDocument doc;
    if (deserializeJson(doc, text.str())) exit(2);
    if (!doc.isNull()) {
        s.alerts.clear();
        for (JsonVariantConst a : doc.as<JsonArrayConst>()) s.alerts.push_back(a.as<const char*>());
    }
    SolarScreen screen;
    screen.callSign = "N0CALL";
    screen.latitude = 34.0901f;  // as stored in the installation record
    screen.longitude = -118.4065f;
    renderSolar(Frame{px.data()}, s, screen, now, [&](Frame f) {
        ArtResampler r;
        return r.begin(f, SUN_RECT, sunCrop(sun.width, sun.height), sun.width, sun.height) && feedBlocks(sun, r)
            && (r.finish(), true);
    });
    writePpm(dir + "/" + name + ".ppm", px);
}

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
    for (const char* name : {"art-cover-aic", "art-wide-cleveland", "art-wide-met"})  // standalone_render.ART_CASES
        renderArtCase(argv[1], name, px);
    const Image sun = readPpm(std::string(argv[1]) + "/solar-sun-input.ppm");
    for (const char* name : {"solar-quiet", "solar-alerts", "solar-long-alert"})  // standalone_render.SOLAR_CASES
        renderSolarCase(argv[1], name, sun, px);
    return 0;
}
