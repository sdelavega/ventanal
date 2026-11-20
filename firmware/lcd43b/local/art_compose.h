// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
//
// Standalone art frames, following the server: art/compose.py compose() and
// devices/lcd43b.py render_art(). The JPEG decoder streams blocks in raster
// order into an area-averaging resampler that keeps only a band of rows, so
// no full-size image is ever held. Unusually shaped pieces get the server's
// blurred, darkened backdrop (built at 1/8 scale here) with the whole piece
// contained on top. The credit is drawn over a soft shade like the server's.
#pragma once

#include "art_sources.h"
#include "shapes.h"
#include "standalone_fonts.h"
#include <cmath>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

namespace lcd::local {

// A buffer whose allocation can fail without throwing: art runs on a board
// with little free memory, where a failed allocation must skip a piece, not
// abort the firmware.
template<class T>
struct Buffer {
    std::unique_ptr<T[]> data;
    size_t size = 0;
    bool allocate(size_t n, const T& fill = T{}) {
        data.reset(new (std::nothrow) T[n]);
        size = data ? n : 0;
        if (data) std::fill(data.get(), data.get() + n, fill);
        return bool(data);
    }
    T& operator[](size_t i) { return data[i]; }
    const T& operator[](size_t i) const { return data[i]; }
};

struct ArtRect {
    int x = 0, y = 0, w = 0, h = 0;
};

// compose(): cover crop when the piece is within 15 % of 4:3, otherwise a
// contained piece over a blurred cover.
struct ComposePlan {
    bool cover = true;
    ArtRect crop;     // source pixels that fill the screen (ImageOps.fit, centred)
    ArtRect contain;  // where the whole piece goes when !cover (ImageOps.contain, centred)
};

inline ComposePlan planCompose(int width, int height) {
    ComposePlan p;
    const double target = double(ART_WIDTH) / ART_HEIGHT, aspect = double(width) / height;
    if (aspect > target) {
        const int w = int(std::lround(height * target));
        p.crop = {(width - w) / 2, 0, w, height};
    } else {
        const int h = int(std::lround(width / target));
        p.crop = {0, (height - h) / 2, width, h};
    }
    const double ratio = aspect / (4.0 / 3.0);
    p.cover = ratio >= 1 / 1.15 && ratio <= 1.15;
    int w = ART_WIDTH, h = ART_HEIGHT;
    if (aspect > target) h = int(std::lround(double(height) / width * ART_WIDTH));
    else if (aspect < target) w = int(std::lround(double(width) / height * ART_HEIGHT));
    p.contain = {(ART_WIDTH - w) / 2, (ART_HEIGHT - h) / 2, w, h};
    return p;
}

// Area-average a source crop onto a destination rectangle of the frame, fed
// with RGB888 blocks in raster order (a JPEG decoder's MCUs, left to right,
// then down). Each destination pixel averages the source pixels of its box
// (at least one, so a small piece is enlarged by repetition).
class ArtResampler {
public:
    bool begin(Frame frame, ArtRect dest, ArtRect crop, int sourceWidth, int sourceHeight, int blockRows = 16) {
        frame_ = frame;
        dest_ = dest;
        crop_ = crop;
        sourceWidth_ = sourceWidth;
        if (dest.w <= 0 || dest.h <= 0 || crop.w <= 0 || crop.h <= 0 || crop.x + crop.w > sourceWidth
            || crop.y + crop.h > sourceHeight)
            return false;
        if (!rowFirst_.allocate(size_t(sourceHeight), int16_t(-1)) || !rowLast_.allocate(size_t(sourceHeight), int16_t(-2))
            || !rowEnd_.allocate(size_t(dest.h)) || !colFirst_.allocate(size_t(sourceWidth), int16_t(-1))
            || !colLast_.allocate(size_t(sourceWidth), int16_t(-2)))
            return false;
        for (int ty = 0; ty < dest.h; ++ty) {
            const int y0 = crop.y + int(int64_t(ty) * crop.h / dest.h);
            const int y1 = std::max(y0 + 1, crop.y + int(int64_t(ty + 1) * crop.h / dest.h));
            rowEnd_[size_t(ty)] = y1;
            for (int y = y0; y < y1 && y < sourceHeight; ++y) {
                if (rowFirst_[size_t(y)] < 0) rowFirst_[size_t(y)] = ty;
                rowLast_[size_t(y)] = ty;
            }
        }
        for (int tx = 0; tx < dest.w; ++tx) {
            const int x0 = crop.x + int(int64_t(tx) * crop.w / dest.w);
            const int x1 = std::max(x0 + 1, crop.x + int(int64_t(tx + 1) * crop.w / dest.w));
            for (int x = x0; x < x1 && x < sourceWidth; ++x) {
                if (colFirst_[size_t(x)] < 0) colFirst_[size_t(x)] = tx;
                colLast_[size_t(x)] = tx;
            }
        }
        // Rows one band of source rows can touch, plus the rows still open.
        bandRows_ = int(int64_t(blockRows + 2) * dest.h / crop.h) + 4;
        nextRow_ = 0;
        bandY_ = -1;
        return sums_.allocate(size_t(bandRows_) * size_t(dest.w));
    }

    // One block: w x h RGB888 pixels at (x, y) of the source.
    void block(int x, int y, int w, int h, const uint8_t* rgb) {
        if (y > bandY_) {
            flushBefore(y);
            bandY_ = y;
        }
        for (int row = 0; row < h; ++row) {
            const int sy = y + row;
            if (sy < 0 || sy >= int(rowFirst_.size) || rowFirst_[size_t(sy)] < 0) continue;
            for (int col = 0; col < w; ++col) {
                const int sx = x + col;
                if (sx < 0 || sx >= sourceWidth_ || colFirst_[size_t(sx)] < 0) continue;
                const uint8_t* p = rgb + (size_t(row) * w + col) * 3;
                for (int ty = rowFirst_[size_t(sy)]; ty <= rowLast_[size_t(sy)]; ++ty) {
                    if (ty < nextRow_ || ty >= nextRow_ + bandRows_) continue;
                    Sum* line = &sums_[size_t(ty % bandRows_) * size_t(dest_.w)];
                    for (int tx = colFirst_[size_t(sx)]; tx <= colLast_[size_t(sx)]; ++tx) {
                        Sum& s = line[tx];
                        if (s.n == 256) continue;  // enough for an average; keeps 16-bit sums exact
                        s.r = uint16_t(s.r + p[0]);
                        s.g = uint16_t(s.g + p[1]);
                        s.b = uint16_t(s.b + p[2]);
                        ++s.n;
                    }
                }
            }
        }
    }

    void finish() { flushBefore(1 << 30); }

private:
    struct Sum {  // 8 bytes: a band of 20 rows of 800 is 128 KB
        uint16_t r = 0, g = 0, b = 0, n = 0;
    };

    void flushBefore(int sourceRow) {
        while (nextRow_ < dest_.h && rowEnd_[size_t(nextRow_)] <= sourceRow) {
            Sum* line = &sums_[size_t(nextRow_ % bandRows_) * size_t(dest_.w)];
            const int y = dest_.y + nextRow_;
            if (y >= frame_.first && y < frame_.first + frame_.rows) {
                uint16_t* out = frame_.px + size_t(y - frame_.first) * FRAME_WIDTH + dest_.x;
                for (int tx = 0; tx < dest_.w; ++tx) {
                    const Sum& s = line[tx];
                    const uint32_t n = s.n ? s.n : 1;
                    out[tx] = rgb565(((s.r + n / 2) / n) << 16 | ((s.g + n / 2) / n) << 8 | ((uint32_t(s.b) + n / 2) / n));
                }
            }
            std::fill(line, line + dest_.w, Sum{});  // ready for the row bandRows_ later
            ++nextRow_;
        }
    }

    Frame frame_{nullptr};
    ArtRect dest_, crop_;
    int sourceWidth_ = 0, bandRows_ = 0, nextRow_ = 0, bandY_ = -1;
    Buffer<int16_t> rowFirst_, rowLast_, colFirst_, colLast_;
    Buffer<int> rowEnd_;
    Buffer<Sum> sums_;
};

namespace art_detail {

inline void expand565(uint16_t c, int& r, int& g, int& b) {
    const int r5 = (c >> 11) & 0x1f, g6 = (c >> 5) & 0x3f, b5 = c & 0x1f;
    r = (r5 << 3) | (r5 >> 2);
    g = (g6 << 2) | (g6 >> 4);
    b = (b5 << 3) | (b5 >> 2);
}

// Three box passes approximate a Gaussian of standard deviation sigma.
inline bool blurFloat(Buffer<float>& v, int w, int h, int stride, double sigma) {
    const int radius = std::max(1, int(std::lround(std::sqrt(4 * sigma * sigma + 1) / 2.0 - 0.5)));
    Buffer<float> tmp;
    if (!tmp.allocate(size_t(std::max(w, h)))) return false;
    for (int pass = 0; pass < 3; ++pass) {
        for (int y = 0; y < h; ++y) {  // rows
            float* row = &v[size_t(y) * stride];
            for (int x = 0; x < w; ++x) {
                float s = 0;
                for (int k = -radius; k <= radius; ++k) s += row[std::min(w - 1, std::max(0, x + k))];
                tmp[size_t(x)] = s / float(2 * radius + 1);
            }
            std::copy(tmp.data.get(), tmp.data.get() + w, row);
        }
        for (int x = 0; x < w; ++x) {  // columns
            for (int y = 0; y < h; ++y) {
                float s = 0;
                for (int k = -radius; k <= radius; ++k) s += v[size_t(std::min(h - 1, std::max(0, y + k))) * stride + x];
                tmp[size_t(y)] = s / float(2 * radius + 1);
            }
            for (int y = 0; y < h; ++y) v[size_t(y) * stride + x] = tmp[size_t(y)];
        }
    }
    return true;
}

}  // namespace art_detail

// compose()'s backdrop: the cover image blurred (radius 60 * 800 / 1600 = 30)
// and darkened to 55 %, built at 1/8 scale and enlarged bilinearly.
inline bool blurredBackdrop(Frame f) {
    constexpr int s = 8, w = ART_WIDTH / s, h = ART_HEIGHT / s;
    Buffer<float> ch[3];
    for (auto& c : ch)
        if (!c.allocate(size_t(w) * h)) return false;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            int sr = 0, sg = 0, sb = 0;
            for (int dy = 0; dy < s; ++dy)
                for (int dx = 0; dx < s; ++dx) {
                    int r, g, b;
                    art_detail::expand565(f.px[size_t(y * s + dy) * FRAME_WIDTH + x * s + dx], r, g, b);
                    sr += r;
                    sg += g;
                    sb += b;
                }
            ch[0][size_t(y * w + x)] = sr / float(s * s);
            ch[1][size_t(y * w + x)] = sg / float(s * s);
            ch[2][size_t(y * w + x)] = sb / float(s * s);
        }
    for (auto& c : ch)
        if (!art_detail::blurFloat(c, w, h, w, 30.0 / s)) return false;
    for (int y = 0; y < ART_HEIGHT; ++y) {
        const float fy = std::min(float(h - 1), std::max(0.f, (y + 0.5f) / s - 0.5f));
        const int y0 = int(fy), y1 = std::min(h - 1, y0 + 1);
        const float ty = fy - y0;
        for (int x = 0; x < ART_WIDTH; ++x) {
            const float fx = std::min(float(w - 1), std::max(0.f, (x + 0.5f) / s - 0.5f));
            const int x0 = int(fx), x1 = std::min(w - 1, x0 + 1);
            const float tx = fx - x0;
            uint32_t rgb = 0;
            for (int c = 0; c < 3; ++c) {
                const auto& v = ch[c];
                const float top = v[size_t(y0 * w + x0)] * (1 - tx) + v[size_t(y0 * w + x1)] * tx;
                const float bottom = v[size_t(y1 * w + x0)] * (1 - tx) + v[size_t(y1 * w + x1)] * tx;
                const int value = std::min(255, std::max(0, int(std::lround((top * (1 - ty) + bottom * ty) * 0.55f))));
                rgb = rgb << 8 | uint32_t(value);
            }
            f.px[size_t(y) * FRAME_WIDTH + x] = rgb565(rgb);
        }
    }
    return true;
}

struct ArtCredit {
    std::string title, artist, date, museum;
};

// render_art()'s credit: title (Inter Bold 12, at most 60 % of 772 px) and
// "   artist · date · museum" (Inter 11), dropping museum then date before
// fitting, over a blurred shade.
inline void drawCredit(Frame f, const ArtCredit& credit) {
    std::string title = renderable(InterBold12, credit.title.c_str());
    if (title.empty()) title = "Untitled";
    constexpr int available = 772;
    title = fitText64(InterBold12, title.c_str(), int32_t(available * 0.6 * 64));
    const int32_t title64 = textWidth64(InterBold12, title.c_str());
    const int remaining = available - title64 / 64;
    std::vector<std::string> fields = {renderable(Inter11, credit.artist.c_str()), renderable(Inter11, credit.date.c_str()),
                                       renderable(Inter11, credit.museum.c_str())};
    std::string secondary;
    while (true) {
        secondary.clear();
        for (const std::string& v : fields)
            if (!v.empty()) secondary += (secondary.empty() ? "" : " \xc2\xb7 ") + v;
        if (!secondary.empty()) secondary = "   " + secondary;
        if (textWidth64(Inter11, secondary.c_str()) <= int32_t(remaining) * 64 || fields.size() == 1) break;
        fields.pop_back();
    }
    secondary = fitText(Inter11, secondary.c_str(), remaining);
    const int32_t width64 = title64 + textWidth64(Inter11, secondary.c_str());

    // The shade: rounded_rectangle((0, 458, 14 + width + 20, 480), 15, fill=150), GaussianBlur(7).
    constexpr int top = 437, bottom = ART_HEIGHT;  // 458 - 3 sigma
    const int right = int(std::lround(14 + width64 / 64.0 + 20));
    const int maskWidth = std::min(ART_WIDTH, right + 22);
    const int rows = bottom - top;
    Buffer<float> mask;
    const bool shade = mask.allocate(size_t(maskWidth) * rows, 0.f);  // without memory: text without shade
    for (int y = 458; shade && y < bottom; ++y)
        for (int x = 0; x <= right && x < maskWidth; ++x) {
            bool inside = true;
            const int r = 15;
            auto corner = [&](int cx, int cy) { return (x - cx) * (x - cx) + (y - cy) * (y - cy) <= r * r; };
            if (x < r && y < 458 + r) inside = corner(r, 458 + r);
            else if (x > right - r && y < 458 + r) inside = corner(right - r, 458 + r);
            else if (x < r && y > 480 - r) inside = corner(r, 480 - r);
            else if (x > right - r && y > 480 - r) inside = corner(right - r, 480 - r);
            if (inside) mask[size_t(y - top) * maskWidth + x] = 150.f;
        }
    const bool blurred = shade && art_detail::blurFloat(mask, maskWidth, rows, maskWidth, 7.0);
    for (int y = top; blurred && y < bottom; ++y) {
        if (y < f.first || y >= f.first + f.rows) continue;
        uint16_t* line = f.px + size_t(y - f.first) * FRAME_WIDTH;
        for (int x = 0; x < maskWidth; ++x) {
            const float m = mask[size_t(y - top) * maskWidth + x];
            if (m < 0.5f) continue;
            int r, g, b;
            art_detail::expand565(line[x], r, g, b);
            const float keep = 1.f - m / 255.f;
            line[x] = rgb565(uint32_t(std::lround(r * keep)) << 16 | uint32_t(std::lround(g * keep)) << 8
                             | uint32_t(std::lround(b * keep)));
        }
    }
    drawTextRaw64(f, InterBold12, title.c_str(), 14 * 64, 474, 0xffffff);
    drawTextRaw64(f, Inter11, secondary.c_str(), 14 * 64 + title64, 474, 0xffffff);
}

// The whole art frame. decode(resampler) streams the source image's blocks
// into the resampler and returns false if decoding fails; it runs once for a
// cover fit and twice for a contained piece (backdrop, then the piece).
template<class Decode>
inline bool composeArt(Frame f, int width, int height, Decode decode, const ArtCredit& credit) {
    if (width <= 0 || height <= 0) return false;
    const ComposePlan plan = planCompose(width, height);
    ArtResampler resampler;
    if (!resampler.begin(f, {0, 0, ART_WIDTH, ART_HEIGHT}, plan.crop, width, height) || !decode(resampler))
        return false;
    resampler.finish();
    if (!plan.cover) {
        if (!blurredBackdrop(f)) return false;
        ArtResampler piece;
        if (!piece.begin(f, plan.contain, {0, 0, width, height}, width, height) || !decode(piece)) return false;
        piece.finish();
    }
    drawCredit(f, credit);
    return true;
}

}  // namespace lcd::local
