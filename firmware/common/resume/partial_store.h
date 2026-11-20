// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
#pragma once

#include "../../lcd43b/sha256.h"  // Portable SHA-256 shared with LCD frames.
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

struct PartialMeta {
    char url[256];
    char validator[96];
    char wireValidator[72];  // Optional HTTP validator for a pinned source.
    uint32_t total;  // Zero until a response establishes the full length.
    uint32_t have;
    uint32_t touchedAt;
};

// VRP1 keeps the original frame sidecar layout. VRP2 holds the longer
// signed OTA identity and its independent HTTP validator.
inline constexpr size_t kPartialMetaV1Bytes = 348;
inline constexpr size_t kPartialMetaBytes = 444;
using PartialMetaBytes = std::vector<uint8_t>;

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
inline constexpr size_t kV1ValidatorBytes = 72;
static_assert(kPartialMetaV1Bytes == kValidatorOffset + kV1ValidatorBytes + 16);
static_assert(kPartialMetaBytes == kValidatorOffset + sizeof(PartialMeta::validator) +
               sizeof(PartialMeta::wireValidator) + 16);

}  // namespace resume_detail

inline PartialMetaBytes encodePartialMeta(const PartialMeta& meta) {
    using namespace resume_detail;
    const bool v2 = *meta.wireValidator || !std::memchr(meta.validator, 0, kV1ValidatorBytes);
    const size_t validatorBytes = v2 ? sizeof(meta.validator) : kV1ValidatorBytes;
    const size_t totalOffset = kValidatorOffset + validatorBytes + (v2 ? sizeof(meta.wireValidator) : 0);
    const size_t crcOffset = totalOffset + 12;
    PartialMetaBytes bytes(crcOffset + 4);
    std::memcpy(bytes.data(), v2 ? "VRP2" : "VRP1", kUrlOffset);
    std::memcpy(bytes.data() + kUrlOffset, meta.url, sizeof(meta.url));
    std::memcpy(bytes.data() + kValidatorOffset, meta.validator, validatorBytes);
    if (v2) std::memcpy(bytes.data() + kValidatorOffset + validatorBytes,
                        meta.wireValidator, sizeof(meta.wireValidator));
    writeLe32(bytes.data() + totalOffset, meta.total);
    writeLe32(bytes.data() + totalOffset + 4, meta.have);
    writeLe32(bytes.data() + totalOffset + 8, meta.touchedAt);
    writeLe32(bytes.data() + crcOffset, metaCrc(bytes.data(), crcOffset));
    return bytes;
}

inline bool decodePartialMeta(const uint8_t* bytes, size_t size, PartialMeta& meta) {
    using namespace resume_detail;
    const bool v2 = size == kPartialMetaBytes && std::memcmp(bytes, "VRP2", kUrlOffset) == 0;
    if (!v2 && (size != kPartialMetaV1Bytes || std::memcmp(bytes, "VRP1", kUrlOffset) != 0)) return false;
    const size_t validatorBytes = v2 ? sizeof(meta.validator) : kV1ValidatorBytes;
    const size_t wireOffset = kValidatorOffset + validatorBytes;
    const size_t totalOffset = size - 16;
    const size_t crcOffset = size - 4;
    if (readLe32(bytes + crcOffset) != metaCrc(bytes, crcOffset) ||
        !std::memchr(bytes + kUrlOffset, 0, sizeof(meta.url)) ||
        !std::memchr(bytes + kValidatorOffset, 0, validatorBytes) ||
        (v2 && !std::memchr(bytes + wireOffset, 0, sizeof(meta.wireValidator)))) return false;

    const uint32_t total = readLe32(bytes + totalOffset);
    const uint32_t have = readLe32(bytes + totalOffset + 4);
    if (total && have > total) return false;
    meta = {};
    std::memcpy(meta.url, bytes + kUrlOffset, sizeof(meta.url));
    std::memcpy(meta.validator, bytes + kValidatorOffset, validatorBytes);
    if (v2) std::memcpy(meta.wireValidator, bytes + wireOffset, sizeof(meta.wireValidator));
    meta.total = total;
    meta.have = have;
    meta.touchedAt = readLe32(bytes + totalOffset + 8);
    return true;
}

using HashFeed = std::function<void()>;
inline constexpr size_t kPartialHashBlockBytes = 1024;

class PartialStore {
public:
    virtual bool readMeta(const char* target, PartialMeta&) = 0;
    virtual bool writeMeta(const char* target, const PartialMeta&) = 0;
    // Successful append must persist bytes before writeMeta publishes the new offset.
    virtual bool append(const char* target, const uint8_t*, size_t) = 0;
    // A store may retain the last published checkpoint after a failed write.
    virtual bool keepAfterWriteFailure(const char*, uint32_t) { return false; }
    virtual bool truncate(const char* target, uint32_t size) = 0;
    // Report the size of <target>.part, never the committed target.
    virtual int64_t size(const char* target) = 0;
    // Atomically replace target with <target>.part and retire the sidecar.
    virtual bool commit(const char* target) = 0;
    virtual void discard(const char* target) = 0;
    // Reset the payload for a fresh response without forgiving a zero-byte storage strike.
    virtual void resetPartial(const char* target) { discard(target); }
    virtual bool sha256(const char* target, uint8_t out[32], const HashFeed& feed = {}) = 0;
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

    bool sha256(const char* target, uint8_t out[32], const HashFeed& feed = {}) override {
        const auto found = partials_.find(target);
        if (found == partials_.end()) return false;
        const auto& bytes = found->second;
        Sha256 hash;
        for (size_t offset = 0; offset < bytes.size();) {
            if (feed) feed();
            const size_t count = std::min(bytes.size() - offset, kPartialHashBlockBytes);
            hash.update(bytes.data() + offset, count);
            offset += count;
        }
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
