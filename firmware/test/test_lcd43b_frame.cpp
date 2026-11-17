// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega

// VNR1 frame validation and SHA-256 verification, including the bundled
// digest implementation against published FIPS 180-4 test vectors.

#include "../lcd43b/frame.h"
#include "../lcd43b/sha256.h"
#include <cassert>
#include <cstdio>
#include <cstring>

namespace {

const char* hexOf(const uint8_t* digest) {
    static char text[65];
    static const char HEX[] = "0123456789abcdef";
    for (int i = 0; i < 32; ++i) {
        text[2 * i] = HEX[digest[i] >> 4];
        text[2 * i + 1] = HEX[digest[i] & 0x0F];
    }
    text[64] = '\0';
    return text;
}

uint8_t frame[lcd::FRAME_BYTES];

void buildFrame() {
    memcpy(frame, "VNR1", 4);
    frame[4] = 0x20; frame[5] = 0x03;   // 800 little-endian
    frame[6] = 0xE0; frame[7] = 0x01;   // 480 little-endian
    memset(frame + 8, 0, 8);
    for (size_t i = 0; i < lcd::WIDTH * lcd::HEIGHT; ++i) {
        const uint16_t value = uint16_t((i * 2654435761u) & 0xFFFF);
        frame[lcd::HEADER_BYTES + 2 * i] = uint8_t(value);
        frame[lcd::HEADER_BYTES + 2 * i + 1] = uint8_t(value >> 8);
    }
}

}  // namespace

int main() {
    assert(lcd::FRAME_BYTES == 768016);
    assert(lcd::HEADER_BYTES == 16);
    {   // Published FIPS 180-4 vectors, one-shot and incremental.
        Sha256 hash;
        hash.update(reinterpret_cast<const uint8_t*>("abc"), 3);
        assert(!strcmp(hexOf(hash.digest()),
                       "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
        hash.reset();
        assert(!strcmp(hexOf(hash.digest()),
                       "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
        hash.reset();
        const char* longText = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
        const size_t length = strlen(longText);
        for (size_t i = 0; i < length; ++i)
            hash.update(reinterpret_cast<const uint8_t*>(longText + i), 1);
        assert(!strcmp(hexOf(hash.digest()),
                       "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"));
    }
    buildFrame();
    {   // Header, size and hash verification of a deterministic frame.
        assert(lcd::frameVerified(frame, lcd::FRAME_BYTES,
                                  "40a610f7001620b8f617a4e85e209af4187d33b05d6c7f37ea4c9d0a03bb7170"));
        uint8_t damaged[lcd::FRAME_BYTES];
        memcpy(damaged, frame, sizeof damaged);
        damaged[lcd::FRAME_BYTES - 1] ^= 0x01;
        assert(!lcd::frameVerified(damaged, lcd::FRAME_BYTES,
                                  "40a610f7001620b8f617a4e85e209af4187d33b05d6c7f37ea4c9d0a03bb7170"));
        assert(!lcd::frameVerified(frame, lcd::FRAME_BYTES - 1,
                                   "40a610f7001620b8f617a4e85e209af4187d33b05d6c7f37ea4c9d0a03bb7170"));
        assert(!lcd::frameVerified(frame, lcd::FRAME_BYTES, "40A610F7"));
        assert(!lcd::frameVerified(frame, lcd::FRAME_BYTES, nullptr));
    }
    {   // Header field validation.
        assert(lcd::headerValid(frame));
        uint8_t header[lcd::HEADER_BYTES];
        memcpy(header, frame, sizeof header);
        assert(lcd::headerValid(header));
        header[0] = 'X';
        assert(!lcd::headerValid(header));
        memcpy(header, frame, sizeof header);
        header[5] = 0x04;   // wrong width
        assert(!lcd::headerValid(header));
        memcpy(header, frame, sizeof header);
        header[7] = 0x02;   // wrong height
        assert(!lcd::headerValid(header));
        memcpy(header, frame, sizeof header);
        header[12] = 1;     // reserved byte set
        assert(!lcd::headerValid(header));
    }
    puts("lcd43b frame: FIPS digests, VNR1 header, hash verification passed");
    return 0;
}