// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
//
// Shapes for the board's own screens, matching the server's LCDDraw
// (server/src/ventanal/devices/lcd_draw.py): straight rectangles and lines
// are crisp; rounded rectangles and ellipses are drawn at 4x and reduced.
// The server reduces with Lanczos, the board with a 4x4 average: measured on
// these shapes, the two differ by at most 20/255 per pixel.
#pragma once

#include "../lcars_geometry.h"
#include "text.h"
#include <algorithm>

namespace lcd::local {

// Pillow-inclusive rectangle [x0, x1] x [y0, y1], no anti-aliasing.
inline void fillRect(Frame f, int x0, int y0, int x1, int y1, uint32_t rgb) {
    const uint16_t c = rgb565(rgb);
    x0 = std::max(x0, 0);
    x1 = std::min(x1, FRAME_WIDTH - 1);
    for (int y = std::max(y0, f.first); y <= y1 && y < f.first + f.rows && y < FRAME_HEIGHT; ++y)
        std::fill(f.px + size_t(y - f.first) * FRAME_WIDTH + x0, f.px + size_t(y - f.first) * FRAME_WIDTH + x1 + 1, c);
}

inline void clearFrame(Frame f) {
    std::fill(f.px, f.px + size_t(f.rows) * FRAME_WIDTH, uint16_t(0));
}

namespace detail {

// An ellipse with Pillow's inclusive bounding box [a0, a1] x [b0, b1] in 4x
// space covers the continuous box [a0, a1 + 1] x [b0, b1 + 1].
struct Oval {
    double cx, cy, rx, ry;
    Oval(double a0, double b0, double a1, double b1)
        : cx((a0 + a1 + 1) / 2), cy((b0 + b1 + 1) / 2), rx((a1 - a0 + 1) / 2), ry((b1 - b0 + 1) / 2) {}
    bool contains(double x, double y) const {
        const double dx = (x - cx) / rx, dy = (y - cy) / ry;
        return dx * dx + dy * dy <= 1.0;
    }
};

template<class Inside>
void coverShape(Frame f, int x0, int y0, int x1, int y1, uint32_t rgb, Inside inside) {
    for (int y = std::max({y0, f.first, 0}); y <= y1 && y < f.first + f.rows && y < FRAME_HEIGHT; ++y) {
        uint16_t* line = f.px + size_t(y - f.first) * FRAME_WIDTH;
        for (int x = std::max(x0, 0); x <= x1 && x < FRAME_WIDTH; ++x) {
            int count = 0;
            for (int sy = 0; sy < 4; ++sy)
                for (int sx = 0; sx < 4; ++sx) count += inside(x * 4 + sx + 0.5, y * 4 + sy + 0.5);
            if (count) line[x] = blend565(line[x], rgb, unsigned((count * 15 + 8) / 16));
        }
    }
}

}  // namespace detail

// LCDDraw.rounded_rectangle((x0, y0, x1, y1), radius): inclusive pixels.
inline void roundedRect(Frame f, int x0, int y0, int x1, int y1, int radius, uint32_t rgb) {
    const double X0 = x0 * 4, Y0 = y0 * 4, X1 = x1 * 4 + 3, Y1 = y1 * 4 + 3;  // 4x, inclusive
    const int d = std::min({radius * 8, int(X1 - X0), int(Y1 - Y0)});           // corner circle box
    const detail::Oval tl(X0, Y0, X0 + d, Y0 + d), tr(X1 - d, Y0, X1, Y0 + d);
    const detail::Oval bl(X0, Y1 - d, X0 + d, Y1), br(X1 - d, Y1 - d, X1, Y1);
    detail::coverShape(f, x0, y0, x1, y1, rgb, [&](double x, double y) {
        if (x < X0 || x > X1 + 1 || y < Y0 || y > Y1 + 1) return false;
        if (x < tl.cx && y < tl.cy) return tl.contains(x, y);
        if (x > tr.cx && y < tr.cy) return tr.contains(x, y);
        if (x < bl.cx && y > bl.cy) return bl.contains(x, y);
        if (x > br.cx && y > br.cy) return br.contains(x, y);
        return true;
    });
}

// LCDDraw.ellipse((x0, y0, x1, y1)): inclusive pixels.
inline void ellipse(Frame f, int x0, int y0, int x1, int y1, uint32_t rgb) {
    const detail::Oval oval(x0 * 4, y0 * 4, x1 * 4 + 3, y1 * 4 + 3);
    detail::coverShape(f, x0, y0, x1, y1, rgb, [&](double x, double y) { return oval.contains(x, y); });
}

// Spans baked by tools/standalone_assets.py (exact server pixels).
template<size_t N>
inline void drawSpans(Frame f, const ChromeSpan (&spans)[N]) {
    for (const ChromeSpan& s : spans)
        if (s.y >= f.first && s.y < f.first + f.rows)
            std::fill(f.px + size_t(s.y - f.first) * FRAME_WIDTH + s.left,
                      f.px + size_t(s.y - f.first) * FRAME_WIDTH + s.right + 1, s.colour);
}

}  // namespace lcd::local
