// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
//
// Text for the board's own screens, placed the way the server's Pillow code
// places it (see tools/standalone_assets.py): 1/64 px advances and kerning,
// the pen rounded to a pixel at each glyph, anchors snapped to pixels, glyph
// coverage blended over what is already drawn. renderable() and fitText()
// follow server/src/ventanal/text.py.
#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace lcd::local {

struct TextGlyph {
    uint32_t offset;   // into the face's 4-bit coverage bitmap (two pixels per byte)
    uint8_t w, h;
    int8_t x0, y0;     // bitmap origin relative to the pen and baseline
    uint16_t advance64;
};

struct KernPair {
    uint16_t left, right;
    int16_t kern64;
};

struct TextFace {
    const uint8_t* bits;
    const uint16_t* codes;  // sorted code points
    const TextGlyph* glyphs;
    uint16_t count;
    const KernPair* kerns;  // sorted by (left, right)
    uint16_t kernCount;
};

inline constexpr int FRAME_WIDTH = 800, FRAME_HEIGHT = 480;

// A full RGB565 frame, or the band [first, first + rows) of one.
struct Frame {
    uint16_t* px;
    int first = 0;
    int rows = FRAME_HEIGHT;
};

enum class Anchor { LeftBaseline, MiddleBaseline, RightBaseline };  // Pillow 'ls', 'ms', 'rs'

// ---- colour ---------------------------------------------------------------

inline constexpr uint16_t rgb565(uint32_t rgb) {
    return uint16_t(((rgb >> 19) & 0x1f) << 11 | ((rgb >> 10) & 0x3f) << 5 | ((rgb >> 3) & 0x1f));
}

// Mixes rgb (0xRRGGBB) over an RGB565 pixel with coverage a/15, in 8-bit
// channels like Pillow, then back to RGB565 by truncation like the frame encoder.
inline uint16_t blend565(uint16_t bg, uint32_t rgb, unsigned a) {
    if (a >= 15) return rgb565(rgb);
    if (!a) return bg;
    const unsigned br = (bg >> 11) & 0x1f, bgr = (bg >> 5) & 0x3f, bb = bg & 0x1f;
    const unsigned r8 = (br << 3) | (br >> 2), g8 = (bgr << 2) | (bgr >> 4), b8 = (bb << 3) | (bb >> 2);
    auto mix = [a](unsigned back, unsigned front) { return (back * (15 - a) + front * a + 7) / 15; };
    const unsigned r = mix(r8, (rgb >> 16) & 0xff), g = mix(g8, (rgb >> 8) & 0xff), b = mix(b8, rgb & 0xff);
    return uint16_t((r >> 3) << 11 | (g >> 2) << 5 | (b >> 3));
}

// ---- UTF-8 ----------------------------------------------------------------

inline std::vector<uint32_t> decodeUtf8(const char* s) {
    std::vector<uint32_t> out;
    const auto* p = reinterpret_cast<const unsigned char*>(s ? s : "");
    while (*p) {
        uint32_t c = *p++;
        int more = c >= 0xf0 ? 3 : c >= 0xe0 ? 2 : c >= 0xc0 ? 1 : 0;
        if (more) c &= 0x3f >> more;
        for (; more && (*p & 0xc0) == 0x80; --more) c = (c << 6) | (*p++ & 0x3f);
        out.push_back(more ? 0xfffd : c);
    }
    return out;
}

inline std::string encodeUtf8(const std::vector<uint32_t>& codes) {
    std::string out;
    for (uint32_t c : codes) {
        if (c < 0x80) {
            out += char(c);
        } else if (c < 0x800) {
            out += char(0xc0 | c >> 6);
            out += char(0x80 | (c & 0x3f));
        } else if (c < 0x10000) {
            out += char(0xe0 | c >> 12);
            out += char(0x80 | ((c >> 6) & 0x3f));
            out += char(0x80 | (c & 0x3f));
        } else {
            out += char(0xf0 | c >> 18);
            out += char(0x80 | ((c >> 12) & 0x3f));
            out += char(0x80 | ((c >> 6) & 0x3f));
            out += char(0x80 | (c & 0x3f));
        }
    }
    return out;
}

// ---- metrics --------------------------------------------------------------

inline const TextGlyph* findGlyph(const TextFace& face, uint32_t code) {
    size_t lo = 0, hi = face.count;
    while (lo < hi) {
        const size_t mid = (lo + hi) / 2;
        if (face.codes[mid] == code) return &face.glyphs[mid];
        if (face.codes[mid] < code) lo = mid + 1;
        else hi = mid;
    }
    return nullptr;
}

inline int kern64(const TextFace& face, uint32_t left, uint32_t right) {
    size_t lo = 0, hi = face.kernCount;
    while (lo < hi) {
        const size_t mid = (lo + hi) / 2;
        const KernPair& k = face.kerns[mid];
        if (k.left == left && k.right == right) return k.kern64;
        if (k.left < left || (k.left == left && k.right < right)) lo = mid + 1;
        else hi = mid;
    }
    return 0;
}

inline int32_t textWidth64(const TextFace& face, const std::vector<uint32_t>& codes) {
    int32_t width = 0;
    uint32_t previous = 0;
    for (uint32_t c : codes) {
        const TextGlyph* g = findGlyph(face, c);
        if (!g) continue;
        if (previous) width += kern64(face, previous, c);
        width += g->advance64;
        previous = c;
    }
    return width;
}

inline int32_t textWidth64(const TextFace& face, const char* utf8) {
    return textWidth64(face, decodeUtf8(utf8));
}

inline bool isSpace(uint32_t c) { return c == ' ' || (c >= '\t' && c <= '\r'); }

// text.py renderable(): drop glyphs the face lacks, collapse whitespace.
inline std::string renderable(const TextFace& face, const char* utf8) {
    std::vector<uint32_t> out;
    bool pendingSpace = false;
    for (uint32_t c : decodeUtf8(utf8)) {
        if (isSpace(c)) {
            pendingSpace = !out.empty();
        } else if (findGlyph(face, c)) {
            if (pendingSpace) out.push_back(' ');
            pendingSpace = false;
            out.push_back(c);
        }
    }
    return encodeUtf8(out);
}

// text.py fit(): the longest prefix (stripped of trailing ' ' and '·') plus
// an ellipsis that fits maxWidth pixels, or "" when even "…" does not fit.
inline std::string fitText(const TextFace& face, const char* utf8, int maxWidth) {
    constexpr uint32_t ellipsis = 0x2026;
    const std::vector<uint32_t> codes = decodeUtf8(utf8);
    const int32_t limit = int32_t(maxWidth) * 64;
    if (textWidth64(face, codes) <= limit) return encodeUtf8(codes);
    if (textWidth64(face, std::vector<uint32_t>{ellipsis}) > limit) return "";
    auto candidate = [&](size_t n) {
        std::vector<uint32_t> prefix(codes.begin(), codes.begin() + long(n));
        while (!prefix.empty() && (prefix.back() == ' ' || prefix.back() == 0xb7)) prefix.pop_back();
        prefix.push_back(ellipsis);
        return prefix;
    };
    size_t lo = 0, hi = codes.size();
    while (lo < hi) {
        const size_t mid = (lo + hi + 1) / 2;
        if (textWidth64(face, candidate(mid)) <= limit) lo = mid;
        else hi = mid - 1;
    }
    return encodeUtf8(candidate(lo));
}

// ---- drawing --------------------------------------------------------------

inline int32_t floorDiv64(int32_t v) { return v >= 0 ? v / 64 : -((-v + 63) / 64); }

// Draws text the way the server's _text() helpers do: renderable(), then
// fitText() when maxWidth >= 0, then Pillow's placement for the anchor.
inline void drawText(Frame frame, const TextFace& face, const char* utf8, int x, int y, uint32_t rgb,
                     Anchor anchor = Anchor::LeftBaseline, int maxWidth = -1) {
    std::string label = renderable(face, utf8);
    if (maxWidth >= 0) label = fitText(face, label.c_str(), maxWidth);
    const std::vector<uint32_t> codes = decodeUtf8(label.c_str());
    const int32_t width = textWidth64(face, codes);
    int32_t pen = x * 64;
    if (anchor == Anchor::MiddleBaseline) pen = (x - (width / 2 + 32) / 64) * 64;
    if (anchor == Anchor::RightBaseline) pen = floorDiv64(x * 64 - width + 32) * 64;
    uint32_t previous = 0;
    for (uint32_t c : codes) {
        const TextGlyph* g = findGlyph(face, c);
        if (!g) continue;
        if (previous) pen += kern64(face, previous, c);
        previous = c;
        const int left = floorDiv64(pen + 32) + g->x0, top = y + g->y0;
        pen += g->advance64;
        for (int row = 0; row < g->h; ++row) {
            const int py = top + row;
            if (py < frame.first || py >= frame.first + frame.rows || py >= FRAME_HEIGHT) continue;
            uint16_t* line = frame.px + size_t(py - frame.first) * FRAME_WIDTH;
            for (int col = 0; col < g->w; ++col) {
                const int px = left + col;
                if (px < 0 || px >= FRAME_WIDTH) continue;
                const size_t i = size_t(row) * g->w + size_t(col);
                const uint8_t pair = face.bits[g->offset + i / 2];
                const unsigned a = i % 2 ? pair & 0x0f : pair >> 4;
                if (a) line[px] = blend565(line[px], rgb, a);
            }
        }
    }
}

}  // namespace lcd::local
