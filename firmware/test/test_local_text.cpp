// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
#include "../lcd43b/local/standalone_fonts.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace lcd::local;

// Expected values from the server: font(name, size).getlength(...) * 64,
// renderable() and fit() (server/src/ventanal/text.py) on the same strings.
static void widths_match_pillow() {
    assert(textWidth64(AntonioBold30, "ATMOSPHERIC CONDITIONS") == 18102);
    assert(textWidth64(Antonio13, "BEVERLY HILLS \xc2\xb7 CA \xc2\xb7 34.09N 118.41W") == 10670);
    assert(textWidth64(Antonio14, "12PM") == 1706);
    assert(textWidth64(Antonio16, "WIND SW 9 MPH") == 5741);
}

static void renderable_matches_server() {
    assert(renderable(Antonio16, "  WIND\tSW  9 MPH ") == "WIND SW 9 MPH");
    assert(renderable(Antonio16, "caf\xc3\xa9 \xe2\x98\x83 ok") == "caf ok");  // glyphs the face lacks are dropped
    assert(renderable(Antonio16, "") == "");
}

static void fit_matches_server() {
    assert(fitText(AntonioBold12, "ADVISORY \xc2\xb7 FLOOD WATCH UNTIL Thu 6:00 PM", 100)
           == "ADVISORY \xc2\xb7 FLOOD WA\xe2\x80\xa6");
    assert(fitText(AntonioBold24, "THUNDERSTORM WITH HEAVY HAIL", 204) == "THUNDERSTORM WITH\xe2\x80\xa6");
    assert(fitText(Antonio14, "12PM", 34) == "12PM");
    assert(fitText(AntonioBold16, "Wide text that cannot fit", 5).empty());
    assert(fitText(AntonioBold16, "", 5).empty());
    assert(textWidth64(AntonioBold12, "ADVISORY \xc2\xb7 FLOOD WA\xe2\x80\xa6") == 6274);
}

static std::vector<uint16_t> canvas(uint16_t fill = 0) { return std::vector<uint16_t>(800 * 480, fill); }

static int inkLeft(const std::vector<uint16_t>& px, int y0, int y1) {
    for (int x = 0; x < 800; ++x)
        for (int y = y0; y < y1; ++y)
            if (px[size_t(y) * 800 + x]) return x;
    return -1;
}

static int inkRight(const std::vector<uint16_t>& px, int y0, int y1) {
    for (int x = 799; x >= 0; --x)
        for (int y = y0; y < y1; ++y)
            if (px[size_t(y) * 800 + x]) return x;
    return -1;
}

static void anchors_snap_to_pixels() {
    // The same text drawn left-, middle- and right-anchored lands where the
    // width says: 'ms' starts at x - (L/2 + 32)/64, 'rs' at (64x - L + 32)/64.
    const char* text = "11AM";
    const int L = textWidth64(Antonio14, text);
    auto left = canvas(), middle = canvas(), right = canvas();
    drawText(Frame{left.data()}, Antonio14, text, 300, 200, 0xffffff, Anchor::LeftBaseline);
    drawText(Frame{middle.data()}, Antonio14, text, 300, 200, 0xffffff, Anchor::MiddleBaseline);
    drawText(Frame{right.data()}, Antonio14, text, 300, 200, 0xffffff, Anchor::RightBaseline);
    const int base = inkLeft(left, 180, 205);
    assert(base >= 300 && base <= 302);
    assert(inkLeft(middle, 180, 205) == base - (L / 2 + 32) / 64);
    assert(inkLeft(right, 180, 205) == base - 300 + (300 * 64 - L + 32) / 64);
    assert(inkRight(left, 180, 205) < 300 + (L + 63) / 64 + 2);
}

static void text_blends_over_the_background() {
    auto px = canvas(0x001f);  // blue
    drawText(Frame{px.data()}, AntonioBold30, "I", 100, 100, 0xffffff, Anchor::LeftBaseline);
    bool solid = false, edge = false;
    for (int y = 70; y < 101; ++y)
        for (int x = 95; x < 115; ++x) {
            const uint16_t c = px[size_t(y) * 800 + x];
            solid = solid || c == 0xffff;
            edge = edge || (c != 0xffff && c != 0x001f);
        }
    assert(solid && edge);  // full coverage is the ink, partial blends with the blue
}

static void clipped_at_the_frame_edges() {
    auto px = canvas();
    drawText(Frame{px.data()}, AntonioBold108, "88", 760, 40, 0xffffff, Anchor::LeftBaseline);  // runs off right and top
    drawText(Frame{px.data()}, AntonioBold108, "88", -40, 500, 0xffffff, Anchor::LeftBaseline);  // off left and bottom
    assert(inkLeft(px, 0, 480) >= 0);  // drew something, wrote nothing outside the frame
}

int main() {
    widths_match_pillow();
    renderable_matches_server();
    fit_matches_server();
    anchors_snap_to_pixels();
    text_blends_over_the_background();
    clipped_at_the_frame_edges();
    puts("local text: widths, renderable, fit, anchors, blending and clipping match the server's rules");
}
