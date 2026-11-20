// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
#pragma once

#include "../../lcd43b/sha256.h"  // Portable SHA-256 shared with LCD frames.
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <utility>
#include <vector>

struct PartialMeta {
    char url[256];
    char validator[72];
    uint32_t total;
    uint32_t have;
    uint32_t touchedAt;
};

// <target>.meta format: VRP1, URL, validator, total, have, touchedAt, CRC32.
// The strings occupy their full fixed widths. Integers are little-endian and
// the IEEE CRC32 covers every byte before the CRC field.
inline constexpr size_t kPartialMetaBytes = 348;
using PartialMetaBytes = std::array<uint8_t, kPartialMetaBytes>;

namespace resume_detail {

inline void writeLe32(uint8_t* dst, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) dst[i] = static_cast<uint8_t>(value >> (8 * i));
}

inline uint32_t readLe32(const uint8_t* src) {
    uint32_t value = 0;
    for (unsigned i = 0; i < 4; ++i) value |= uint32_t(src[i]) << (8 * i);
    return value;
}

inline uint32_t metaCrc(const uint8_t* data, size_t size) {
    uint32_t crc = UINT32_MAX;
    for (size_t i = 0; i < size; ++i) {
        crc ^= data[i];
        for (unsigned bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ ((crc & 1) ? 0xedb88320u : 0);
        }
    }
    return ~crc;
}

inline constexpr size_t kUrlOffset = 4;
inline constexpr size_t kValidatorOffset = kUrlOffset + sizeof(PartialMeta::url);
inline constexpr size_t kTotalOffset = kValidatorOffset + sizeof(PartialMeta::validator);
inline constexpr size_t kCrcOffset = kTotalOffset + 12;
static_assert(kPartialMetaBytes == kCrcOffset + 4);

}  // namespace resume_detail

inline PartialMetaBytes encodePartialMeta(const PartialMeta& meta) {
    using namespace resume_detail;
    PartialMetaBytes bytes{};
    std::memcpy(bytes.data(), "VRP1", kUrlOffset);
    std::memcpy(bytes.data() + kUrlOffset, meta.url, sizeof(meta.url));
    std::memcpy(bytes.data() + kValidatorOffset, meta.validator, sizeof(meta.validator));
    writeLe32(bytes.data() + kTotalOffset, meta.total);
    writeLe32(bytes.data() + kTotalOffset + 4, meta.have);
    writeLe32(bytes.data() + kTotalOffset + 8, meta.touchedAt);
    writeLe32(bytes.data() + kCrcOffset, metaCrc(bytes.data(), kCrcOffset));
    return bytes;
}

inline bool decodePartialMeta(const uint8_t* bytes, size_t size, PartialMeta& meta) {
    using namespace resume_detail;
    if (size != kPartialMetaBytes || std::memcmp(bytes, "VRP1", kUrlOffset) != 0 ||
        readLe32(bytes + kCrcOffset) != metaCrc(bytes, kCrcOffset) ||
        !std::memchr(bytes + kUrlOffset, 0, sizeof(meta.url)) ||
        !std::memchr(bytes + kValidatorOffset, 0, sizeof(meta.validator))) return false;

    const uint32_t total = readLe32(bytes + kTotalOffset);
    const uint32_t have = readLe32(bytes + kTotalOffset + 4);
    if (have > total) return false;
    std::memcpy(meta.url, bytes + kUrlOffset, sizeof(meta.url));
    std::memcpy(meta.validator, bytes + kValidatorOffset, sizeof(meta.validator));
    meta.total = total;
    meta.have = have;
    meta.touchedAt = readLe32(bytes + kTotalOffset + 8);
    return true;
}

class PartialStore {
public:
    virtual bool readMeta(const char* target, PartialMeta&) = 0;
    virtual bool writeMeta(const char* target, const PartialMeta&) = 0;
    // Successful append must persist bytes before writeMeta publishes the new offset.
    virtual bool append(const char* target, const uint8_t*, size_t) = 0;
    virtual bool truncate(const char* target, uint32_t size) = 0;
    virtual int64_t size(const char* target) = 0;
    // Atomically replace target with <target>.part and retire the sidecar.
    virtual bool commit(const char* target) = 0;
    virtual void discard(const char* target) = 0;
    virtual bool sha256(const char* target, uint8_t out[32]) = 0;
    virtual ~PartialStore() = default;
};

class MemoryStore : public PartialStore {
public:
    bool readMeta(const char* target, PartialMeta& meta) override {
        const auto found = metadata_.find(target);
        return found != metadata_.end() && decodePartialMeta(found->second.data(), found->second.size(), meta);
    }

    bool writeMeta(const char* target, const PartialMeta& meta) override {
        metadata_[target] = encodePartialMeta(meta);
        return true;
    }

    bool append(const char* target, const uint8_t* data, size_t count) override {
        auto& partial = partials_[target];
        if (count) partial.insert(partial.end(), data, data + count);
        return true;
    }

    bool truncate(const char* target, uint32_t size) override {
        const auto found = partials_.find(target);
        if (found == partials_.end() || size > found->second.size()) return false;
        found->second.resize(size);
        return true;
    }

    int64_t size(const char* target) override {
        const auto found = partials_.find(target);
        return found == partials_.end() ? -1 : static_cast<int64_t>(found->second.size());
    }

    bool commit(const char* target) override {
        const auto found = partials_.find(target);
        if (found == partials_.end()) return false;
        committed_[target] = std::move(found->second);
        discard(target);
        return true;
    }

    void discard(const char* target) override {
        partials_.erase(target);
        metadata_.erase(target);
    }

    bool sha256(const char* target, uint8_t out[32]) override {
        const auto found = partials_.find(target);
        if (found == partials_.end()) return false;
        Sha256 hash;
        hash.update(found->second.data(), found->second.size());
        std::memcpy(out, hash.digest(), 32);
        return true;
    }

    const std::vector<uint8_t>* committed(const char* target) const {
        const auto found = committed_.find(target);
        return found == committed_.end() ? nullptr : &found->second;
    }

private:
    std::map<std::string, PartialMetaBytes> metadata_;
    std::map<std::string, std::vector<uint8_t>> partials_;
    std::map<std::string, std::vector<uint8_t>> committed_;
};
