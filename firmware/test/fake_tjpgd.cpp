// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
// Use libjpeg in place of the ESP32 ROM. Both the file and buffer tests
// exercise the production TJpgDec callbacks, including seeks over markers.
#include <rom/tjpgd.h>

#include <cassert>
#include <csetjmp>
#include <cstdio>
#include <jpeglib.h>

namespace {

struct Error {
    jpeg_error_mgr base;
    jmp_buf exit;
};

struct Source {
    jpeg_source_mgr base;
    JDEC* decoder;
    UINT (*input)(JDEC*, BYTE*, UINT);
    BYTE buffer[512];
};

Source& source(j_decompress_ptr jpeg) {
    return *reinterpret_cast<Source*>(jpeg->src);
}

void abortDecode(j_common_ptr jpeg) {
    longjmp(reinterpret_cast<Error*>(jpeg->err)->exit, 1);
}

boolean fill(j_decompress_ptr jpeg) {
    auto& src = source(jpeg);
    src.base.bytes_in_buffer = src.input(src.decoder, src.buffer, sizeof src.buffer);
    if (!src.base.bytes_in_buffer) abortDecode(reinterpret_cast<j_common_ptr>(jpeg));
    src.base.next_input_byte = src.buffer;
    return TRUE;
}

void skip(j_decompress_ptr jpeg, long count) {
    if (count <= 0) return;
    auto& src = source(jpeg);
    const auto length = static_cast<size_t>(count);
    if (length <= src.base.bytes_in_buffer) {
        src.base.next_input_byte += length;
        src.base.bytes_in_buffer -= length;
        return;
    }
    const UINT beyondBuffer = UINT(length - src.base.bytes_in_buffer);
    if (src.input(src.decoder, nullptr, beyondBuffer) != beyondBuffer)
        abortDecode(reinterpret_cast<j_common_ptr>(jpeg));
    src.base.next_input_byte = nullptr;
    src.base.bytes_in_buffer = 0;
}

}  // namespace

JRESULT jd_prepare(JDEC* decoder, UINT (*input)(JDEC*, BYTE*, UINT), void*, UINT, void* device) {
    decoder->device = device;
    jpeg_decompress_struct jpeg{};
    Error error{};
    jpeg.err = jpeg_std_error(&error.base);
    error.base.error_exit = abortDecode;
    if (setjmp(error.exit)) {
        jpeg_destroy_decompress(&jpeg);
        return JDR_INP;
    }
    jpeg_create_decompress(&jpeg);
    Source src{};
    src.decoder = decoder;
    src.input = input;
    src.base.init_source = src.base.term_source = [](j_decompress_ptr) {};
    src.base.fill_input_buffer = fill;
    src.base.skip_input_data = skip;
    src.base.resync_to_restart = jpeg_resync_to_restart;
    jpeg.src = &src.base;
    jpeg_read_header(&jpeg, TRUE);
    jpeg.out_color_space = JCS_RGB;
    jpeg_start_decompress(&jpeg);
    decoder->width = jpeg.output_width;
    decoder->height = jpeg.output_height;
    decoder->rgb.resize(size_t(decoder->width) * decoder->height * 3);
    while (jpeg.output_scanline < decoder->height) {
        auto* row = decoder->rgb.data() + size_t(jpeg.output_scanline) * decoder->width * 3;
        jpeg_read_scanlines(&jpeg, &row, 1);
    }
    jpeg_finish_decompress(&jpeg);
    jpeg_destroy_decompress(&jpeg);
    return JDR_OK;
}

JRESULT jd_decomp(JDEC* decoder, UINT (*output)(JDEC*, void*, JRECT*), BYTE scale) {
    assert(scale == 0);
    for (UINT y = 0; y < decoder->height; ++y) {
        JRECT row{0, decoder->width - 1, y, y};
        if (!output(decoder, decoder->rgb.data() + size_t(y) * decoder->width * 3, &row))
            return JDR_INTR;
    }
    return JDR_OK;
}
