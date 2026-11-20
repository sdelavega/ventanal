// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
#include "local_media.h"

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <miniz.h>           // the ROM's miniz (compressor), not src/miniz (inflate only)
#include <rom/tjpgd.h>

namespace lcd {
namespace {

struct GrowBuffer {
    uint8_t* data = nullptr;
    size_t size = 0, cap = 0;
    bool failed = false;
    bool append(const void* bytes, size_t n) {
        if (size + n > cap) {
            size_t next = cap ? cap * 2 : 128 * 1024;
            while (next < size + n) next *= 2;
            auto* grown = static_cast<uint8_t*>(heap_caps_realloc(data, next, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
            if (!grown) return !(failed = true);
            data = grown;
            cap = next;
        }
        memcpy(data + size, bytes, n);
        size += n;
        return true;
    }
};

mz_bool putBuffer(const void* bytes, int length, void* user) {
    return static_cast<GrowBuffer*>(user)->append(bytes, size_t(length)) ? MZ_TRUE : MZ_FALSE;
}

struct JpegSession {
    const uint8_t* data;
    size_t length, position;
    local::ArtResampler* resampler;
    Watchdog* watchdog;
    uint32_t started = 0, budgetMs = UINT32_MAX;
    fs::File* file = nullptr;
};

UINT jpegInput(JDEC* jd, BYTE* buffer, UINT bytes) {
    auto* s = static_cast<JpegSession*>(jd->device);
    const size_t n = std::min<size_t>(bytes, s->length - s->position);
    if (s->file) {
        if (buffer) {
            if (s->file->read(buffer, n) != n) return 0;
        } else if (!s->file->seek(s->position + n)) {
            return 0;
        }
        s->watchdog->feed();
    } else if (buffer) {
        memcpy(buffer, s->data + s->position, n);
    }
    s->position += n;
    return UINT(n);
}

UINT jpegOutput(JDEC* jd, void* bitmap, JRECT* rect) {
    auto* s = static_cast<JpegSession*>(jd->device);
    if (rect->left == 0) {
        s->watchdog->feed();
        if (millis() - s->started >= s->budgetMs) return 0;
    }
    s->resampler->block(rect->left, rect->top, rect->right - rect->left + 1, rect->bottom - rect->top + 1,
                        static_cast<const uint8_t*>(bitmap));
    return 1;
}

constexpr size_t JpegPool = 4096;

}  // namespace

bool deflateFrame(const uint8_t* frame, FrameBody& body, Watchdog& watchdog) {
    auto* compressor = static_cast<tdefl_compressor*>(
        heap_caps_malloc(sizeof(tdefl_compressor), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!compressor) return false;
    GrowBuffer out;
    bool ok = tdefl_init(compressor, putBuffer, &out, TDEFL_WRITE_ZLIB_HEADER | 16) == TDEFL_STATUS_OKAY;
    constexpr size_t Chunk = 32 * 1024;
    for (size_t offset = 0; ok && offset < FRAME_BYTES; offset += Chunk) {
        const size_t n = std::min(Chunk, FRAME_BYTES - offset);
        const bool last = offset + n == FRAME_BYTES;
        const tdefl_status status =
            tdefl_compress_buffer(compressor, frame + offset, n, last ? TDEFL_FINISH : TDEFL_NO_FLUSH);
        ok = !out.failed && (last ? status == TDEFL_STATUS_DONE : status == TDEFL_STATUS_OKAY);
        watchdog.feed();
    }
    heap_caps_free(compressor);
    if (!ok || !validWireSize(FrameEncoding::Zlib, out.size)) {
        heap_caps_free(out.data);
        return false;
    }
    body = FrameBody{std::shared_ptr<uint8_t>(out.data, heap_caps_free), out.size};
    return true;
}

bool composeArtJpeg(const uint8_t* jpeg, size_t length, local::Frame f, const local::ArtCredit& credit,
                    Watchdog& watchdog, uint32_t budgetMs) {
    std::unique_ptr<uint8_t, decltype(&heap_caps_free)> pool(
        static_cast<uint8_t*>(heap_caps_malloc(JpegPool, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)), heap_caps_free);
    if (!pool) return false;
    JpegSession session{jpeg, length, 0, nullptr, &watchdog, millis(), budgetMs};
    JDEC jd{};
    if (jd_prepare(&jd, jpegInput, pool.get(), JpegPool, &session) != JDR_OK) return false;  // progressive: JDR_FMT3
    // Decode at the smallest scale (1/2^s) whose crop still covers the screen.
    const int width = int(jd.width), height = int(jd.height);
    int scale = 0;
    while (scale < 3) {
        const local::ComposePlan plan = local::planCompose(width >> (scale + 1), height >> (scale + 1));
        if (plan.crop.w < local::ART_WIDTH || plan.crop.h < local::ART_HEIGHT) break;
        ++scale;
    }
    const auto decode = [&](local::ArtResampler& resampler) {
        session.position = 0;
        session.resampler = &resampler;
        JDEC pass{};
        return jd_prepare(&pass, jpegInput, pool.get(), JpegPool, &session) == JDR_OK
            && jd_decomp(&pass, jpegOutput, BYTE(scale)) == JDR_OK;
    };
    // The S3 ROM clips the edge MCU before shifting its dimensions down.
    const int scaledWidth = width >> scale, scaledHeight = height >> scale;
    return local::composeArt(f, scaledWidth, scaledHeight, decode, credit);
}

bool drawSunJpeg(fs::File& jpeg, local::Frame f, Watchdog& watchdog) {
    std::unique_ptr<uint8_t, decltype(&heap_caps_free)> pool(
        static_cast<uint8_t*>(heap_caps_malloc(JpegPool, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)), heap_caps_free);
    if (!pool) return false;
    local::ArtResampler resampler;
    JpegSession session{nullptr, jpeg.size(), 0, &resampler, &watchdog, millis(), UINT32_MAX, &jpeg};
    JDEC jd{};
    if (jd_prepare(&jd, jpegInput, pool.get(), JpegPool, &session) != JDR_OK) return false;
    const int width = int(jd.width), height = int(jd.height);
    const int scale = local::sunJpegScale(width, height);
    // The S3 ROM clips the edge MCU before shifting its dimensions down.
    const int scaledWidth = width >> scale, scaledHeight = height >> scale;
    if (!resampler.begin(f, local::SUN_RECT, local::sunCrop(scaledWidth, scaledHeight), scaledWidth, scaledHeight))
        return false;
    session.position = 0;
    if (!jpeg.seek(0)) return false;
    JDEC pass{};
    if (jd_prepare(&pass, jpegInput, pool.get(), JpegPool, &session) != JDR_OK
        || jd_decomp(&pass, jpegOutput, BYTE(scale)) != JDR_OK)
        return false;
    resampler.finish();
    return true;
}

size_t artBodyHeaderBytes(const local::ArtCredit& c) {
    return 8 + c.title.size() + c.artist.size() + c.date.size() + c.museum.size() + 4;
}

void writeArtBodyHeader(uint8_t* body, const local::ArtCredit& c) {
    const uint32_t creditBytes = uint32_t(artBodyHeaderBytes(c) - 8);
    memcpy(body, ArtBodyMagic, 4);
    for (int i = 0; i < 4; ++i) body[4 + i] = uint8_t(creditBytes >> (8 * i));
    uint8_t* p = body + 8;
    for (const std::string* s : {&c.title, &c.artist, &c.date, &c.museum}) {
        memcpy(p, s->c_str(), s->size() + 1);
        p += s->size() + 1;
    }
}

bool isArtBody(const uint8_t* body, size_t length) {
    return length > 8 && !memcmp(body, ArtBodyMagic, 4);
}

bool composeArtBody(const uint8_t* body, size_t length, uint8_t* frame, Watchdog& watchdog) {
    if (!isArtBody(body, length)) return false;
    const uint32_t creditBytes = uint32_t(body[4]) | uint32_t(body[5]) << 8 | uint32_t(body[6]) << 16
        | uint32_t(body[7]) << 24;
    if (creditBytes > length - 8 || body[8 + creditBytes - 1] != '\0') return false;
    local::ArtCredit credit;
    const char* p = reinterpret_cast<const char*>(body + 8);
    const char* end = p + creditBytes;
    for (std::string* s : {&credit.title, &credit.artist, &credit.date, &credit.museum}) {
        if (p >= end) return false;
        *s = p;
        p += s->size() + 1;
    }
    static const uint8_t header[HEADER_BYTES] = {'V', 'N', 'R', '1', 0x20, 0x03, 0xE0, 0x01};
    memcpy(frame, header, HEADER_BYTES);
    auto* pixels = reinterpret_cast<uint16_t*>(frame + HEADER_BYTES);
    std::fill(pixels, pixels + size_t(WIDTH) * HEIGHT, uint16_t(0));
    return composeArtJpeg(body + 8 + creditBytes, length - 8 - creditBytes, local::Frame{pixels}, credit, watchdog);
}

}  // namespace lcd
