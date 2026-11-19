// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
//
// The standalone weather screen, drawn on the board. A transcription of
// render_lcd_weather(standalone=True), weather_layout() and draw_hours_chart()
// in server/src/ventanal/devices/lcd_weather.py; the host check
// (tools/standalone_compare.py) holds it to the server's pixels.
#pragma once

#include "shapes.h"
#include "standalone_chrome.h"
#include "standalone_fonts.h"
#include "weather_data.h"
#include <cmath>
#include <cstdio>
#include <string>

namespace lcd::local {

struct WeatherScreen {
    bool metric = false;
    bool clock24h = false;
    const char* place = "";  // as stored at setup: "City, ST"
    double latitude = NAN, longitude = NAN;
};

namespace weather_detail {

// Python round() on an exact quotient n / d (d > 0): halves go to even.
inline int roundDiv(long long n, long long d) {
    long long q = n / d, r = n % d;
    if (r < 0) {
        r += d;
        --q;
    }
    if (2 * r > d || (2 * r == d && (q & 1))) ++q;
    return int(q);
}

// Python f'{v:.0f}', including "-0" for small negatives.
inline std::string format0(double v) {
    const int n = roundHalfEven(v);
    char text[16];
    snprintf(text, sizeof text, "%s%d", n == 0 && std::signbit(v) && v != 0 ? "-" : "", n);
    return text;
}

inline std::string upper(std::string s) {
    for (char& c : s)
        if (c >= 'a' && c <= 'z') c = char(c - 'a' + 'A');
    return s;
}

inline const WmoName* wmo(int code) {
    for (const WmoName& n : WMO_NAMES)
        if (n.code == code) return &n;
    return nullptr;
}

// _clock(): '%H:%M', or '%I:%M %p' without a leading zero.
inline std::string clock(int hour, int minute, bool h24) {
    char text[16];
    if (h24) snprintf(text, sizeof text, "%02d:%02d", hour, minute);
    else snprintf(text, sizeof text, "%d:%02d %s", hour % 12 ? hour % 12 : 12, minute, hour < 12 ? "AM" : "PM");
    return text;
}

inline std::string hourLabel(int hour, bool h24) {  // _hour_label()
    char text[8];
    if (h24) snprintf(text, sizeof text, "%02d", hour);
    else snprintf(text, sizeof text, "%d%s", hour % 12 ? hour % 12 : 12, hour < 12 ? "AM" : "PM");
    return text;
}

inline const char* weekdayTitle(const LocalDate& d) {  // '%a': "Thu"
    static const char* const names[] = {"Thu", "Fri", "Sat", "Sun", "Mon", "Tue", "Wed"};
    const int64_t days = daysFromCivil(d.year, d.month, d.day);
    return names[((days % 7) + 7) % 7];
}

inline LocalClock localClock(int64_t epoch, int32_t offset) {
    const int64_t t = epoch + offset;
    const int64_t days = (t >= 0 ? t : t - 86399) / 86400;
    const int64_t seconds = t - days * 86400;
    LocalClock c;
    c.date = civilFromDays(days);
    c.hour = uint8_t(seconds / 3600);
    c.minute = uint8_t(seconds / 60 % 60);
    return c;
}

// "Beverly Hills, CA" (setup) -> "Beverly Hills · CA" (the server's geocode name).
inline std::string placeLabel(const char* place) {
    std::string out;
    for (const char* p = place; *p; ++p) {
        if (p[0] == ',' && p[1] == ' ') {
            out += " \xc2\xb7";
            continue;
        }
        out += *p;
    }
    return out;
}

// str.strip(' ·') on UTF-8.
inline std::string stripSpacesAndDots(const std::string& s) {
    std::vector<uint32_t> codes = decodeUtf8(s.c_str());
    auto strip = [](uint32_t c) { return c == ' ' || c == 0xb7; };
    while (!codes.empty() && strip(codes.back())) codes.pop_back();
    size_t first = 0;
    while (first < codes.size() && strip(codes[first])) ++first;
    return encodeUtf8(std::vector<uint32_t>(codes.begin() + long(first), codes.end()));
}

}  // namespace weather_detail

// weather_layout(): the advisory, wrapped onto at most three lines.
inline std::vector<std::string> advisoryLines(const LocalAlert* alerts, int alertCount, int32_t utcOffset,
                                              bool clock24h) {
    using namespace weather_detail;
    std::string advisory = "NO ACTIVE ADVISORIES";
    if (alertCount > 0) {
        advisory = "ADVISORY \xc2\xb7 " + upper(alerts[0].event);
        if (alerts[0].hasEnd) {
            const LocalClock ends = localClock(alerts[0].ends, utcOffset);
            advisory += std::string(" UNTIL ") + weekdayTitle(ends.date) + " " + clock(ends.hour, ends.minute, clock24h);
        }
    }
    const std::string text = renderable(AntonioBold12, advisory.c_str());
    std::vector<std::string> words;
    for (size_t start = 0; start < text.size();) {
        const size_t end = text.find(' ', start);
        words.push_back(text.substr(start, end == std::string::npos ? std::string::npos : end - start));
        if (end == std::string::npos) break;
        start = end + 1;
    }
    std::vector<std::string> lines;
    size_t next = 0;
    while (next < words.size() && lines.size() < 2) {
        std::string line = words[next++];
        while (next < words.size() && textWidth64(AntonioBold12, (line + " " + words[next]).c_str()) <= 636 * 64)
            line += " " + words[next++];
        lines.push_back(fitText(AntonioBold12, line.c_str(), 636));
    }
    if (next < words.size()) {
        std::string rest = words[next++];
        while (next < words.size()) rest += " " + words[next++];
        lines.push_back(fitText(AntonioBold12, rest.c_str(), 636));
    }
    return lines;
}

inline void renderWeather(Frame f, const LocalWeather& w, const LocalAlert* alerts, int alertCount,
                          const WeatherScreen& screen, time_t now) {
    using namespace weather_detail;
    clearFrame(f);
    const LocalClock local = localClock(now, w.utcOffsetSeconds);
    char text[96];

    // draw_lcd_frame(sections, accent='violet', clock_block='orange'), labels on the blocks.
    drawSpans(f, WEATHER_CHROME);
    const int yday = int(daysFromCivil(local.date.year, local.date.month, local.date.day)
                         - daysFromCivil(local.date.year, 1, 1)) + 1;
    snprintf(text, sizeof text, "%03d-%02d%02d", yday, local.hour, local.minute);
    const std::string code1 = text;
    snprintf(text, sizeof text, "%02d%02d-%02d%02d", w.sunrise.hour, w.sunrise.minute, w.sunset.hour, w.sunset.minute);
    const std::string code2 = text;
    const std::string labels[] = {code1, "HOURLY", code2, "LONG RANGE"};
    for (int i = 0; i < 4; ++i) {
        const FrameSection& s = WEATHER_SECTIONS[i];
        drawText(f, AntonioBold13, labels[i].c_str(), 92, s.y + s.h - 6, s.labelInk ? colour::white : colour::black,
                 Anchor::RightBaseline, 86);
    }

    drawText(f, AntonioBold30, "ATMOSPHERIC CONDITIONS", 150, 56, colour::gold);
    std::string location = placeLabel(screen.place);
    if (!std::isnan(screen.latitude) && !std::isnan(screen.longitude)) {
        snprintf(text, sizeof text, " \xc2\xb7 %.2f%s %.2f%s", std::fabs(screen.latitude), screen.latitude >= 0 ? "N" : "S",
                 std::fabs(screen.longitude), screen.longitude >= 0 ? "E" : "W");
        location += text;
    }
    drawText(f, Antonio13, stripSpacesAndDots(upper(location)).c_str(), 151, 74, colour::light_gray,
             Anchor::LeftBaseline, 635);
    drawText(f, AntonioBold108, (format0(w.temp) + "\xc2\xb0").c_str(), 148, 190, colour::white, Anchor::LeftBaseline, 204);
    const WmoName* now_name = wmo(w.code);
    drawText(f, AntonioBold24, upper(now_name ? now_name->longName : "Unknown").c_str(), 150, 222, colour::gold,
             Anchor::LeftBaseline, 204);

    std::string rain = "NO RAIN EXPECTED TODAY";
    if (w.rain) {
        rain = "RAIN ";
        if (w.dayCount) rain += std::to_string(w.days[0].precip) + "% ";
        rain += "AFTER " + clock(w.rainOnset.hour, w.rainOnset.minute, screen.clock24h);
    }
    const std::string rows[] = {
        std::string("WIND ") + w.windDir + " " + format0(w.windSpeed) + (screen.metric ? " KM/H" : " MPH"),
        "HUMIDITY " + std::to_string(w.humidity) + "%",
        rain,
        "SUNSET " + clock(w.sunset.hour, w.sunset.minute, screen.clock24h),
    };
    for (int i = 0; i < 4; ++i)
        drawText(f, Antonio16, rows[i].c_str(), 150, 246 + i * 24, i == 3 ? colour::gold : colour::white,
                 Anchor::LeftBaseline, 204);

    // draw_hours_chart(): the box where the host shows radar.
    constexpr int boxX0 = 366, boxY0 = 80, boxX1 = 786, boxY1 = 328, column = 35;
    constexpr int tempTop = 126, tempBottom = 236, barTop = 262, barBottom = 304;
    if (!w.hourCount) {
        drawText(f, Antonio24, "HOURLY FORECAST UNAVAILABLE", (boxX0 + boxX1) / 2, (boxY0 + boxY1) / 2,
                 colour::light_gray, Anchor::MiddleBaseline, boxX1 - boxX0 - 16);
    } else {
        int temps[localChartHours];
        int low = 1 << 30, high = -(1 << 30);
        for (int i = 0; i < w.hourCount; ++i) {
            temps[i] = roundHalfEven(w.hours[i].temp);
            low = std::min(low, temps[i]);
            high = std::max(high, temps[i]);
        }
        const int span = std::max(1, high - low);
        for (int i = 0; i < w.hourCount; ++i) {
            const int cx = boxX0 + 17 + i * column;
            const int cy = tempBottom - roundDiv(static_cast<long long>(temps[i] - low) * (tempBottom - tempTop), span);
            drawText(f, AntonioBold16, (std::to_string(temps[i]) + "\xc2\xb0").c_str(), cx, cy - 9, colour::white,
                     Anchor::MiddleBaseline, column - 1);
            ellipse(f, cx - 4, cy - 4, cx + 4, cy + 4, colour::gold);
            roundedRect(f, cx - 11, barTop, cx + 10, barBottom, 3, colour::gray);
            const int chance = std::max(0, std::min(100, int(w.hours[i].precip)));
            if (chance) {
                const int top = barBottom - std::max(3, roundDiv(static_cast<long long>(barBottom - barTop) * chance, 100));
                roundedRect(f, cx - 11, top, cx + 10, barBottom, 3, colour::sky);
            }
            drawText(f, Antonio14, hourLabel(w.hours[i].time.hour, screen.clock24h).c_str(), cx, 322,
                     colour::light_gray, Anchor::MiddleBaseline, column - 1);
        }
    }

    fillRect(f, 150, 331, 785, 331, colour::gray);
    for (int i = 0; i < std::min(w.dayCount, localForecastDays); ++i) {
        const LocalDay& day = w.days[i];
        const int cx = 195 + i * 91;
        drawText(f, AntonioBold19, weekdayName(day.date), cx, 351, colour::gold, Anchor::MiddleBaseline);
        drawText(f, AntonioBold27, std::to_string(day.hi).c_str(), cx, 377, colour::white, Anchor::MiddleBaseline, 82);
        drawText(f, Antonio17, std::to_string(day.lo).c_str(), cx, 395, colour::light_gray, Anchor::MiddleBaseline, 82);
        const WmoName* name = wmo(day.code);
        drawText(f, Antonio12, name ? name->shortName : "UNKNOWN", cx, 407, colour::white, Anchor::MiddleBaseline, 82);
        roundedRect(f, cx - 30, 410, cx + 29, 413, 2, colour::gray);
        const int chance = std::max(0, std::min(100, int(day.precip)));
        if (chance)
            roundedRect(f, cx - 30, 410, cx - 31 + std::max(1, roundDiv(60LL * chance, 100)), 413, 2, colour::sky);
    }

    const std::vector<std::string> advisory = advisoryLines(alerts, alertCount, w.utcOffsetSeconds, screen.clock24h);
    for (size_t i = 0; i < advisory.size(); ++i)
        drawText(f, AntonioBold12, advisory[i].c_str(), 150, 430 + int(i) * 12,
                 alertCount > 0 ? colour::red : colour::light_gray);
}

}  // namespace lcd::local
