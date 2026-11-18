// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
#pragma once

#include "frame.h"
#include <cstdlib>
#include <cstring>

#ifdef VENTANAL_HOST_TEST
#include <zlib.h>
#else
#include <miniz.h>
#endif

namespace lcd {

// The destination is exactly FRAME_BYTES. On failure its contents are
// unspecified, so decode into scratch before replacing a live frame.
inline bool decodeFrame(FrameEncoding encoding, const uint8_t* input,
                        size_t size, uint8_t* frame) {
    if (!input || !frame || !validWireSize(encoding, size)) return false;
    if (encoding == FrameEncoding::Raw) {
        if (!headerValid(input)) return false;
        if (frame != input) memcpy(frame, input, FRAME_BYTES);
        return true;
    }
#ifdef VENTANAL_HOST_TEST
    z_stream stream{};
    stream.next_in = const_cast<uint8_t*>(input);
    stream.avail_in = static_cast<uInt>(size);
    stream.next_out = frame;
    stream.avail_out = FRAME_BYTES;
    if (inflateInit(&stream) != Z_OK) return false;
    const bool complete = inflate(&stream, Z_FINISH) == Z_STREAM_END
        && stream.avail_in == 0 && stream.total_out == FRAME_BYTES;
    inflateEnd(&stream);
#else
    auto* stream = static_cast<tinfl_decompressor*>(malloc(sizeof(tinfl_decompressor)));
    if (!stream) return false;
    tinfl_init(stream);
    size_t consumed = size;
    size_t written = FRAME_BYTES;
    // Ask for more input rather than allowing end-of-stream zero padding.
    const auto flags = TINFL_FLAG_PARSE_ZLIB_HEADER | TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF
        | TINFL_FLAG_HAS_MORE_INPUT;
    const bool complete = tinfl_decompress(stream, input, &consumed, frame, frame, &written, flags)
        == TINFL_STATUS_DONE && consumed == size && written == FRAME_BYTES;
    free(stream);
#endif
    return complete && headerValid(frame);
}

}  // namespace lcd
