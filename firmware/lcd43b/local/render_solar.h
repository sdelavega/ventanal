// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
//
// The standalone solar screen, drawn on the board. A transcription of
// render_lcd_solar() in server/src/ventanal/devices/lcd_solar.py; the host
// check (tools/standalone_compare.py) holds it to the server's pixels.
#pragma once

#include "art_compose.h"
#include "render_weather.h"
#include "solar_data.h"
#include <string>

namespace lcd::local {

struct SolarScreen {
    const char* callSign = "";
    double latitude = NAN, longitude = NAN;
};

// The sun's place on the screen and the SDO image's crop (crop_sun(): a
// 4.5 % margin all round hides the timestamp).
inline constexpr ArtRect SUN_RECT{582, 84, 186, 186};

inline ArtRect sunCrop(int width, int height) {
    const int margin = roundHalfEven(std::min(width, height) * 0.045);
    return {margin, margin, width - 2 * margin, height - 2 * margin};
}

namespace solar_render {

// Python f'{sfi:.0f}'.
inline std::string whole(double v) { return weather_detail::format0(v); }

inline uint32_t ratingColour(const char* rating) {
    return !strcmp(rating, "Good") ? colour::green : !strcmp(rating, "Fair") ? colour::gold : colour::red;
}

inline uint32_t kColourRgb(double k) {
    switch (kColour(k)) {
    case KColour::Green: return colour::green;
    case KColour::Gold: return colour::gold;
    default: return colour::red;
    }
}

// LCDDraw.rounded_rectangle((x0, top, x1, y1)) with a fractional top: Pillow
// rounds the 4x coordinate (measured 2026-11-19: 340.7 covers 1/4 of row 340).
inline void roundedRectTop(Frame f, int x0, double top, int x1, int y1, int radius, uint32_t rgb) {
    const int q = int(std::floor(top * 4 + 0.5));
    const double X0 = x0 * 4, Y0 = q, X1 = x1 * 4 + 3, Y1 = y1 * 4 + 3;
    const int d = std::min({radius * 8, int(X1 - X0), int(Y1 - Y0)});
    const detail::Oval tl(X0, Y0, X0 + d, Y0 + d), tr(X1 - d, Y0, X1, Y0 + d);
    const detail::Oval bl(X0, Y1 - d, X0 + d, Y1), br(X1 - d, Y1 - d, X1, Y1);
    detail::coverShape(f, x0, q / 4, x1, y1, rgb, [&](double x, double y) {
        if (x < X0 || x > X1 + 1 || y < Y0 || y > Y1 + 1) return false;
        if (x < tl.cx && y < tl.cy) return tl.contains(x, y);
        if (x > tr.cx && y < tr.cy) return tr.contains(x, y);
        if (x < bl.cx && y > bl.cy) return bl.contains(x, y);
        if (x > br.cx && y > br.cy) return br.contains(x, y);
        return true;
    });
}

// image.paste(sun, (582, 84), rounded_mask((186, 186), 93)) over black: the
// sun's pixels are already in place, so this fades them out to a disc.
inline void maskSun(Frame f) {
    const ArtRect r = SUN_RECT;
    const detail::Oval disc(0, 0, r.w * 4 - 1, r.h * 4 - 1);
    for (int y = std::max(r.y, f.first); y < r.y + r.h && y < f.first + f.rows; ++y) {
        uint16_t* line = f.px + size_t(y - f.first) * FRAME_WIDTH;
        for (int x = r.x; x < r.x + r.w; ++x) {
            int count = 0;
            for (int sy = 0; sy < 4; ++sy)
                for (int sx = 0; sx < 4; ++sx)
                    count += disc.contains((x - r.x) * 4 + sx + 0.5, (y - r.y) * 4 + sy + 0.5);
            if (count == 16) continue;
            int red, green, blue;
            art_detail::expand565(line[x], red, green, blue);
            line[x] = count ? blend565(0, uint32_t(red) << 16 | uint32_t(green) << 8 | uint32_t(blue),
                                       unsigned((count * 15 + 8) / 16))
                            : 0;
        }
    }
}

// alert_fit(): keep 18 px type; omit secondary products, then whole words.
inline std::string alertFit(const std::string& message, int width) {
    const TextFace& face = Antonio18;
    const int32_t limit = int32_t(width) * 64;
    auto fits = [&](const std::string& s) { return textWidth64(face, s.c_str()) <= limit; };
    if (fits(message)) return message;
    std::vector<std::string> products;
    for (size_t at = 0;;) {
        const size_t next = message.find(" / ", at);
        products.push_back(message.substr(at, next == std::string::npos ? std::string::npos : next - at));
        if (next == std::string::npos) break;
        at = next + 3;
    }
    while (products.size() > 1) {
        products.pop_back();
        std::string candidate;
        for (size_t i = 0; i < products.size(); ++i) candidate += (i ? " / " : "") + products[i];
        candidate += " \xe2\x80\xa6";
        if (fits(candidate)) return candidate;
    }
    std::vector<std::string> words = solar_detail::split(products[0]);
    while (!words.empty()) {
        words.pop_back();
        std::string candidate;
        for (size_t i = 0; i < words.size(); ++i) candidate += (i ? " " : "") + words[i];
        candidate += words.empty() ? "\xe2\x80\xa6" : " \xe2\x80\xa6";
        if (fits(candidate)) return candidate;
    }
    return fits("\xe2\x80\xa6") ? "\xe2\x80\xa6" : "";
}

// strftime('%a %d') of an ISO date, upper-cased; 'UNKNOWN' when unreadable.
inline std::string forecastDate(const char* iso) {
    LocalClock c;
    std::string text = iso ? iso : "";
    if (text.size() != 10 || !detail::parseClock((text + "T00:00").c_str(), c)) return "UNKNOWN";
    static const char* const days[] = {"THU", "FRI", "SAT", "SUN", "MON", "TUE", "WED"};  // 1970-01-01: Thursday
    const int64_t n = daysFromCivil(c.date.year, c.date.month, c.date.day);
    char out[16];
    snprintf(out, sizeof out, "%s %02d", days[((n % 7) + 7) % 7], c.date.day);
    return out;
}

}  // namespace solar_render

// Draws the screen. `drawSun(Frame)` puts the cropped SDO image into SUN_RECT
// (false when there is none: the disc stays black); `fetched` is the data's
// time, shown in UTC.
template<class DrawSun>
void renderSolar(Frame f, const LocalSolar& s, const SolarScreen& screen, int64_t fetched, DrawSun drawSun) {
    using namespace solar_render;
    using solar_detail::upper;
    clearFrame(f);
    char text[128];

    // draw_lcd_frame(SECTIONS, clock_block='orange'), labels on the blocks.
    drawSpans(f, SOLAR_CHROME);
    static const char* const labels[] = {"SOLAR FLUX", "GEOMAG", "HF BANDS", "KP 24 H", "3-DAY"};
    for (int i = 0; i < 5; ++i) {
        const FrameSection& sec = SOLAR_SECTIONS[i];
        drawText(f, AntonioBold13, labels[i], 92, sec.y + sec.h - 6, sec.labelInk ? colour::white : colour::black,
                 Anchor::RightBaseline, 86);
    }

    drawText(f, AntonioBold30, "SOLAR CONDITIONS", 150, 56, colour::gold);
    const std::string header = std::string(screen.callSign) + " \xc2\xb7 " + gridSquare(screen.latitude, screen.longitude)
        + " \xc2\xb7 " + solar_detail::utcStamp(fetched, true) + " UTC \xc2\xb7 NOAA SWPC \xc2\xb7 NASA SDO";
    drawText(f, Antonio13, upper(header).c_str(), 151, 74, colour::light_gray, Anchor::LeftBaseline, 635);

    drawText(f, Antonio14, "SOLAR FLUX (SFI)", 150, 96, colour::light_gray);
    drawText(f, AntonioBold90, whole(s.sfi).c_str(), 148, 178, colour::white, Anchor::LeftBaseline, 175);
    const int k = integerK(s.k);
    const struct { const char* label; std::string value; int x, y; uint32_t rgb; } boxes[] = {
        {"SUNSPOTS", std::to_string(s.ssn), 340, 96, colour::white},
        {"X-RAY", s.xray, 448, 96, colour::gold},
        {"K-INDEX", std::to_string(k), 340, 153, kColourRgb(k)},
        {"A-INDEX", std::to_string(s.a), 448, 153, colour::white},
    };
    for (const auto& b : boxes) {
        drawText(f, Antonio13, b.label, b.x, b.y, colour::light_gray);
        drawText(f, AntonioBold34, upper(b.value).c_str(), b.x, b.y + 32, b.rgb, Anchor::LeftBaseline, 95);
    }
    const HfEstimate est = conditions(s.sfi, s.k, s.a, s.xray);
    snprintf(text, sizeof text, "FIELD %s \xc2\xb7 EST. NOISE %s \xc2\xb7 MUF \xe2\x89\x88 %d MHZ", est.field, est.noise,
             est.muf);
    drawText(f, Antonio14, text, 150, 201, colour::sky, Anchor::LeftBaseline, 400);

    drawText(f, Antonio13, "DAY", 326, 218, colour::light_gray, Anchor::MiddleBaseline);
    drawText(f, Antonio13, "NIGHT", 462, 218, colour::light_gray, Anchor::MiddleBaseline);
    for (int i = 0; i < 4; ++i) {
        const int y = 224 + i * 21;
        drawText(f, AntonioBold17, upper(est.bands[i].name).c_str(), 150, y + 14, colour::gold, Anchor::LeftBaseline, 100);
        const char* ratings[] = {est.bands[i].day, est.bands[i].night};
        const int xs[] = {270, 406};
        for (int j = 0; j < 2; ++j) {
            roundedRect(f, xs[j], y, xs[j] + 112, y + 17, 9, ratingColour(ratings[j]));
            drawText(f, AntonioBold13, upper(ratings[j]).c_str(), xs[j] + 56, y + 13, colour::black,
                     Anchor::MiddleBaseline);
        }
    }
    drawText(f, Antonio12, "HF CONDITIONS, MUF AND NOISE ARE ESTIMATES", 150, 322, colour::gold, Anchor::LeftBaseline,
             400);

    if (drawSun(f)) maskSun(f);
    drawText(f, Antonio13, "SUN TODAY \xc2\xb7 NASA SDO 171 \xc3\x85", 675, 286, colour::violet, Anchor::MiddleBaseline,
             218);
    std::string scales = "NOAA SCALES";
    static const char keys[] = {'R', 'S', 'G'};
    for (int i = 0; i < 3; ++i)
        scales += std::string(i ? " \xc2\xb7 " : " ") + keys[i] + (s.scaleNow[i].empty() || s.scaleNow[i] == "0" ? "0" : s.scaleNow[i]);
    drawText(f, Antonio13, (scales + " (NOW)").c_str(), 675, 305, colour::white, Anchor::MiddleBaseline, 218);

    // The Kp chart: ticks, eight 3-hour bars, the G1 threshold over them.
    constexpr int x = 172, y = 330, width = 612, height = 23;
    for (int tick : {0, 9}) {
        const int ty = y + height - tick * height / 9;
        fillRect(f, x, ty, x + width, ty, colour::gray);
        drawText(f, Antonio11, std::to_string(tick).c_str(), 164, ty + 4, colour::light_gray, Anchor::RightBaseline);
    }
    for (int i = 0; i < s.kpCount; ++i) {
        const int bx = weather_detail::roundDiv(2 * x + 153LL * i, 2);  // round(x + i * 76.5)
        const double kp = s.kp[i].kp;
        const double barHeight = std::max(2.0, std::min(9.0, std::max(0.0, kp)) / 9 * height);
        roundedRectTop(f, bx, y + height - barHeight, bx + 52, y + height, 2, kColourRgb(kp));
        const std::string hour = std::string(s.kp[i].timeTag).substr(11, 2);
        drawText(f, Antonio11, hour.c_str(), bx + 26, 365, colour::white, Anchor::MiddleBaseline);
    }
    const int threshold = int(y + height - 5.0 / 9 * height);  // 340.2: Pillow's line lands on row 340
    fillRect(f, x, threshold, x + width, threshold, colour::gold);
    drawText(f, Antonio11, "PLANETARY KP \xc2\xb7 3-HOUR INTERVALS \xc2\xb7 UTC", 150, 375, colour::light_gray);
    drawText(f, Antonio11, "G1 (KP 5)", 785, 375, colour::gold, Anchor::RightBaseline);

    for (int i = 0; i < 3; ++i) {
        const ScaleDay& day = s.scales[i + 1];
        const int bx = 150 + i * 214;
        drawText(f, AntonioBold18, forecastDate(day.dateStamp).c_str(), bx, 394, colour::gold, Anchor::LeftBaseline, 204);
        const std::string kpLine = "KP MAX " + kpNotation(s.kpMax[i]) + " \xc2\xb7 " + forecastLabel(s.kpMax[i], day.gScale);
        drawText(f, Antonio15, upper(kpLine).c_str(), bx, 411, colour::white, Anchor::LeftBaseline, 204);
        const std::string chance = "R1\xe2\x80\x93R2 " + (day.hasR ? day.rMinorProb : std::string("?")) + "% \xc2\xb7 S1 "
            + (day.hasS ? day.sProb : std::string("?")) + "%";
        drawText(f, Antonio13, upper(chance).c_str(), bx, 427, colour::light_gray, Anchor::LeftBaseline, 204);
    }
    std::string label, message;
    alertSummary(s.alerts, label, message);
    if (!label.empty()) {
        drawText(f, AntonioBold16, label.c_str(), 150, 450, colour::gold, Anchor::LeftBaseline, 72);
        drawText(f, Antonio18, upper(alertFit(message, 550)).c_str(), 235, 450, colour::white);
    } else {
        drawText(f, Antonio14, message.c_str(), 150, 450, colour::light_gray, Anchor::LeftBaseline, 635);
    }
}

}  // namespace lcd::local
