// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sodium.h>

#include "release_key.h"

struct ReleaseInfo {
    char board[24];
    uint16_t major, minor, patch;
    uint32_t size;
    uint8_t sha256[32];
    uint16_t minMajor, minMinor, minPatch;
};

enum class ReleaseVerdict { Ok, BadSignature, WrongBoard, NotNewer, TooOld, BadSize };

namespace ventanal_release_detail {

// Match the canonical JSON byte format. Literals fix order, field types and
// delimiters, so duplicate fields, whitespace and trailing data are rejected.
struct Cursor {
    const char* next;
    const char* end;

    bool take(const char* expected) {
        const size_t len = strlen(expected);
        if (static_cast<size_t>(end - next) < len || memcmp(next, expected, len) != 0)
            return false;
        next += len;
        return true;
    }

    bool number(uint32_t limit, uint32_t& value) {
        if (next == end || *next < '0' || *next > '9') return false;
        if (*next == '0') {
            ++next;
            value = 0;
            return true;
        }
        value = 0;
        while (next != end && *next >= '0' && *next <= '9') {
            const uint32_t digit = *next++ - '0';
            if (value > (limit - digit) / 10) return false;
            value = value * 10 + digit;
        }
        return true;
    }

    bool version(uint16_t& major, uint16_t& minor, uint16_t& patch) {
        uint32_t a, b, c;
        if (!number(UINT16_MAX, a) || !take(".") || !number(UINT16_MAX, b)
            || !take(".") || !number(UINT16_MAX, c)) return false;
        major = a;
        minor = b;
        patch = c;
        return true;
    }

    static int hex(char c) {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        return -1;
    }

    bool digest(uint8_t* out) {
        for (size_t i = 0; i < 32; ++i) {
            if (end - next < 2) return false;
            const int high = hex(*next++);
            const int low = hex(*next++);
            if (high < 0 || low < 0) return false;
            out[i] = (high << 4) | low;
        }
        return true;
    }

    // The notes URL is server-facing. Validate its canonical string encoding
    // without allocating or retaining it on the board.
    bool notes() {
        while (next != end) {
            const uint8_t c = *next++;
            if (c == '"') return true;
            if (c < 0x20) return false;
            if (c == '\\') {
                if (next == end || *next == '\0') return false;
                if (strchr("\"\\bfnrt", *next)) {
                    ++next;
                    continue;
                }
                if (!take("u00") || end - next < 2) return false;
                const int high = hex(*next++);
                const int low = hex(*next++);
                if (high < 0 || high > 1 || low < 0) return false;
                const int escaped = (high << 4) | low;
                if (escaped == 8 || escaped == 9 || escaped == 10 || escaped == 12 || escaped == 13)
                    return false;  // These controls have shorter canonical escapes.
            } else if (c >= 0x80 && !utf8Tail(c)) {
                return false;
            }
        }
        return false;
    }

    bool utf8Tail(uint8_t lead) {
        if (lead < 0xc2 || lead > 0xf4) return false;
        const int count = lead < 0xe0 ? 1 : (lead < 0xf0 ? 2 : 3);
        if (end - next < count) return false;
        const uint8_t first = *next;
        if ((lead == 0xe0 && first < 0xa0) || (lead == 0xed && first > 0x9f)
            || (lead == 0xf0 && first < 0x90) || (lead == 0xf4 && first > 0x8f))
            return false;
        for (int i = 0; i < count; ++i) {
            if ((static_cast<uint8_t>(*next++) & 0xc0) != 0x80) return false;
        }
        return true;
    }
};

inline uint64_t versionNumber(uint16_t major, uint16_t minor, uint16_t patch) {
    return (static_cast<uint64_t>(major) << 32) | (static_cast<uint32_t>(minor) << 16) | patch;
}

}  // namespace ventanal_release_detail

inline bool parseRelease(const char* json, size_t len, ReleaseInfo& out) {
    if (!json) return false;
    ventanal_release_detail::Cursor cursor{json, json + len};
    ReleaseInfo info{};
    if (!cursor.take("{\"board\":\"")) return false;
    if (cursor.take("inkplate13spectra\"")) {
        strcpy(info.board, "inkplate13spectra");
    } else if (cursor.take("lcd43b\"")) {
        strcpy(info.board, "lcd43b");
    } else {
        return false;
    }
    if (!cursor.take(",\"min_from_version\":\"")
        || !cursor.version(info.minMajor, info.minMinor, info.minPatch)
        || !cursor.take("\",\"notes_url\":\"") || !cursor.notes()
        || !cursor.take(",\"sha256\":\"") || !cursor.digest(info.sha256)
        || !cursor.take("\",\"size\":") || !cursor.number(UINT32_MAX, info.size)
        || !cursor.take(",\"version\":\"") || !cursor.version(info.major, info.minor, info.patch)
        || !cursor.take("\"}") || cursor.next != cursor.end) return false;
    out = info;
    return true;
}

inline bool verifyReleaseSignature(const char* json, size_t len, const char* sigBase64,
                                   const uint8_t publicKey[32]) {
    if (!json || !sigBase64 || !publicKey) return false;
    constexpr size_t encodedSize = sodium_base64_ENCODED_LEN(crypto_sign_BYTES, sodium_base64_VARIANT_ORIGINAL);
    uint8_t signature[crypto_sign_BYTES];
    size_t decodedSize = 0;
    if (strlen(sigBase64) != encodedSize - 1 || sodium_init() < 0
        || sodium_base642bin(signature, sizeof(signature), sigBase64, encodedSize - 1,
                             nullptr, &decodedSize, nullptr, sodium_base64_VARIANT_ORIGINAL) != 0
        || decodedSize != sizeof(signature)) return false;
    char canonical[encodedSize];
    sodium_bin2base64(canonical, sizeof(canonical), signature, sizeof(signature),
                      sodium_base64_VARIANT_ORIGINAL);
    return strcmp(canonical, sigBase64) == 0
        && crypto_sign_verify_detached(signature, reinterpret_cast<const uint8_t*>(json),
                                       len, publicKey) == 0;
}

inline ReleaseVerdict checkRelease(const char* json, size_t len, const char* sig,
                                   const char* board, const char* runningVersion,
                                   uint32_t slotSize, ReleaseInfo& out,
                                   const uint8_t publicKey[32] = VENTANAL_RELEASE_PUBLIC_KEY) {
    using ventanal_release_detail::versionNumber;
    ReleaseInfo info{};
    if (!verifyReleaseSignature(json, len, sig, publicKey)
        || !parseRelease(json, len, info)) return ReleaseVerdict::BadSignature;
    if (!board || strcmp(info.board, board) != 0) return ReleaseVerdict::WrongBoard;
    if (!runningVersion) return ReleaseVerdict::NotNewer;
    ventanal_release_detail::Cursor running{runningVersion, runningVersion + strlen(runningVersion)};
    uint16_t major, minor, patch;
    if (!running.version(major, minor, patch) || running.next != running.end)
        return ReleaseVerdict::NotNewer;
    const auto current = versionNumber(major, minor, patch);
    if (versionNumber(info.major, info.minor, info.patch) <= current)
        return ReleaseVerdict::NotNewer;
    if (current < versionNumber(info.minMajor, info.minMinor, info.minPatch))
        return ReleaseVerdict::TooOld;
    if (info.size == 0 || info.size > slotSize) return ReleaseVerdict::BadSize;
    out = info;
    return ReleaseVerdict::Ok;
}

// Call with the measured size and final SHA-256 of the streamed image.
inline bool releaseImageMatches(const ReleaseInfo& info, size_t size, const uint8_t sha256[32]) {
    return size == info.size && memcmp(info.sha256, sha256, sizeof(info.sha256)) == 0;
}
