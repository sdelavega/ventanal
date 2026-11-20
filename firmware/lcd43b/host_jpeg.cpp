// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
#include "frame_stream.h"
#include "sync.h"

#include <algorithm>
#include <esp_heap_caps.h>
#include <rom/tjpgd.h>

namespace lcd {
namespace {

struct JpegSession {
    const uint8_t* data;
    size_t size;
    uint8_t* frame;
    Watchdog& watchdog;
    fs::File* file = nullptr;
    FsStore* store = nullptr;
    size_t position = 0;
    bool ioError = false;
};

UINT input(JDEC* decoder, BYTE* buffer, UINT bytes) {
    auto& session = *static_cast<JpegSession*>(decoder->device);
    const size_t count = std::min<size_t>(bytes, session.size - session.position);
    if (!count) return 0;
    if (session.file) {
        const bool ok = buffer
            ? readFrameBytes(*session.file, session.store, buffer, count)
            : seekFrame(*session.file, session.store, session.position + count);
        if (!ok) {
            session.ioError = true;
            return 0;
        }
        session.watchdog.feed();
    } else if (buffer) {
        memcpy(buffer, session.data + session.position, count);
    }
    session.position += count;
    return UINT(count);
}

UINT output(JDEC* decoder, void* bitmap, JRECT* rect) {
    auto& session = *static_cast<JpegSession*>(decoder->device);
    if (!rect->left) session.watchdog.feed();
    return writeJpegBlock(session.frame, static_cast<const uint8_t*>(bitmap),
                          rect->left, rect->top, rect->right - rect->left + 1,
                          rect->bottom - rect->top + 1);
}

FrameDecode decode(JpegSession& session) {
    constexpr size_t PoolBytes = 4096;
    std::unique_ptr<uint8_t, decltype(&heap_caps_free)> pool(
        static_cast<uint8_t*>(heap_caps_malloc(PoolBytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
        heap_caps_free);
    if (!pool) return FrameDecode::Memory;
    JDEC decoder{};
    auto result = jd_prepare(&decoder, input, pool.get(), PoolBytes, &session);
    if (result == JDR_OK) {
        if (decoder.width != WIDTH || decoder.height != HEIGHT) return FrameDecode::Invalid;
        result = jd_decomp(&decoder, output, 0);
    }
    if (session.ioError) return FrameDecode::Storage;
    // A ROM pool limit on an already allocated fixed pool is a property
    // of the wire format. Only heap allocation failure can improve on retry.
    if (result != JDR_OK) return FrameDecode::Invalid;
    constexpr uint8_t header[HEADER_BYTES] = {'V', 'N', 'R', '1', 0x20, 0x03, 0xe0, 0x01};
    memcpy(session.frame, header, sizeof header);
    return FrameDecode::Complete;
}

}  // namespace

bool decodeHostJpeg(const uint8_t* data, size_t size, uint8_t* frame, Watchdog& watchdog) {
    if (!frame || !validJpegEnvelope(data, size)) return false;
    JpegSession session{data, size, frame, watchdog};
    return decode(session) == FrameDecode::Complete;
}

FrameDecode decodeHostJpegFile(fs::File& file, FsStore* store, uint8_t* frame, Watchdog& watchdog) {
    const size_t size = file.size();
    if (!frame) return FrameDecode::Memory;
    if (!validWireSize(FrameEncoding::Jpeg, size)) return FrameDecode::Invalid;
    uint8_t start[2], end[2];
    if (!readFrameBytes(file, store, start, sizeof start) ||
        !seekFrame(file, store, size - sizeof end) ||
        !readFrameBytes(file, store, end, sizeof end) ||
        !seekFrame(file, store, 0)) return FrameDecode::Storage;
    if (!isJpegFrame(start, sizeof start) || end[0] != 0xff || end[1] != 0xd9)
        return FrameDecode::Invalid;
    JpegSession session{nullptr, size, frame, watchdog, &file, store};
    return decode(session);
}

}  // namespace lcd
