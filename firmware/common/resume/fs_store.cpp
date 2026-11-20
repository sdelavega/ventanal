// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
#include "fs_store.h"
#include "resume.h"
#include <cerrno>
#include <cstdlib>
#ifdef ARDUINO_ARCH_ESP32
#include <fcntl.h>
#include <unistd.h>
#endif

namespace {
constexpr char kRoot[] = "/partials";
constexpr char kProbe[] = "/partials/.ventanal-sd-probe";
constexpr uint32_t kStaleSeconds = 7 * 86400;

bool hasSuffix(const std::string& name, const char* suffix) {
    if (name.size() <= 5) return false;
    for (size_t i = 0; i < 5; ++i) {
        char c = name[name.size() - 5 + i];
        if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
        if (c != suffix[i]) return false;
    }
    return true;
}

// The Arduino File API has no truncate and its flush/close discard errors.
bool writeSynced(fs::FS& fs, const std::string& path, const uint8_t* data, size_t size, bool append) {
#ifdef ARDUINO_ARCH_ESP32
    const char* mount = fs.mountpoint();
    if (!mount) return false;
    const int fd = ::open((mount + path).c_str(), O_WRONLY | O_CREAT | (append ? O_APPEND : O_TRUNC), 0666);
    if (fd < 0) return false;
    const bool ok = ::write(fd, data, size) == static_cast<ssize_t>(size) && ::fsync(fd) == 0;
    return ::close(fd) == 0 && ok;
#else
    auto file = fs.open(path.c_str(), append ? "a" : "w");
    if (!file) return false;
    const bool ok = file.write(data, size) == size;
    file.flush();
    file.close();
    return ok;
#endif
}
}

FsStore::FsStore(fs::FS& fs, const char* root, void (*ioError)())
    : fs_(fs), root_(root), ioError_(ioError) {}

std::string FsStore::path(const char* target) const {
    if (!target) return {};
    if (*target == '/') ++target;
    std::string name(target);
    if (name.empty() || name.front() == '.' || name.back() == '.' ||
        name.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-") != std::string::npos ||
        hasSuffix(name, ".part") || hasSuffix(name, ".meta")) return {};
    return root_ + '/' + name;
}

bool FsStore::fail() {
    if (!faulted_ && ioError_) {
        faulted_ = true;
        ioError_();
    }
    return false;
}

bool FsStore::ensureRoot() {
    return fs_.exists(root_.c_str()) || fs_.mkdir(root_.c_str()) || fail();
}

fs::File FsStore::openRead(const std::string& path) {
    errno = 0;
    auto file = fs_.open(path.c_str(), "r");
    if (!file && errno != ENOENT) fail();
    return file;
}

bool FsStore::remove(const std::string& path) {
    errno = 0;
    return fs_.remove(path.c_str()) || errno == ENOENT || fail();
}

bool FsStore::readMeta(const char* target, PartialMeta& meta) {
    const auto base = path(target);
    if (faulted_ || base.empty()) return false;
    auto file = openRead(base + ".meta");
    PartialMetaBytes bytes{};
    if (!file || file.size() != bytes.size()) return false;
    if (file.read(bytes.data(), bytes.size()) != bytes.size()) return fail();
    return decodePartialMeta(bytes.data(), bytes.size(), meta);
}

bool FsStore::writeMeta(const char* target, const PartialMeta& meta) {
    const auto base = path(target);
    if (faulted_ || base.empty() || !ensureRoot()) return false;
    const auto bytes = encodePartialMeta(meta);
    return writeSynced(fs_, base + ".meta", bytes.data(), bytes.size(), false) || fail();
}

bool FsStore::append(const char* target, const uint8_t* bytes, size_t count) {
    const auto base = path(target);
    if (faulted_ || base.empty() || !ensureRoot()) return false;
    return writeSynced(fs_, base + ".part", bytes, count, true) || fail();
}

bool FsStore::truncate(const char* target, uint32_t length) {
    const int64_t current = size(target);
    if (current < 0 || current < length) return false;
    if (current == length) return true;
    const auto part = path(target) + ".part";
#ifdef ARDUINO_ARCH_ESP32
    const char* mount = fs_.mountpoint();
    return (mount && ::truncate((mount + part).c_str(), length) == 0) || fail();
#else
    return fs_.truncate(part.c_str(), length) || fail();
#endif
}

int64_t FsStore::size(const char* target) {
    const auto base = path(target);
    if (faulted_ || base.empty()) return -1;
    auto file = openRead(base + ".part");
    return file ? static_cast<int64_t>(file.size()) : -1;
}

bool FsStore::commit(const char* target) {
    const auto base = path(target);
    if (faulted_ || base.empty()) return false;
    const auto part = base + ".part";
    errno = 0;
    if (!fs_.rename(part.c_str(), base.c_str())) {
        if (errno != EEXIST) return fail();
        // FAT requires removing the old file first. The final path is absent
        // between removal and rename, and stays absent if the retry fails.
        if (!remove(base)) return false;
        if (!fs_.rename(part.c_str(), base.c_str())) return fail();
    }
    remove(base + ".meta");
    return true;
}

void FsStore::discard(const char* target) {
    const auto base = path(target);
    if (base.empty()) return;
    remove(base + ".meta");
    remove(base + ".part");
}

bool FsStore::sha256(const char* target, uint8_t out[32]) {
    const auto base = path(target);
    if (faulted_ || base.empty()) return false;
    auto file = openRead(base + ".part");
    if (!file) return false;
    Sha256 hash;
    uint8_t buffer[1024];
    size_t remaining = file.size();
    while (remaining) {
        const size_t count = std::min(remaining, sizeof(buffer));
        if (file.read(buffer, count) != count) return fail();
        hash.update(buffer, count);
        remaining -= count;
    }
    std::memcpy(out, hash.digest(), 32);
    return true;
}

bool FsStore::takeCommitted(const char* target, uint8_t* bytes, size_t size) {
    const auto base = path(target);
    if (base.empty()) return false;
    const bool ok = readFile(base, bytes, size);
    const bool removed = remove(base);
    return (ok && removed) || fail();
}

bool FsStore::readCommitted(const char* target, uint8_t* bytes, size_t size) {
    const auto base = path(target);
    return !faulted_ && !base.empty() && (readFile(base, bytes, size) || fail());
}

bool FsStore::readFile(const std::string& base, uint8_t* bytes, size_t size) {
    auto file = openRead(base);
    bool ok = file && file.size() == size;
    while (ok && size) {
        const size_t count = std::min(size, size_t(kResumeChunk));
        ok = file.read(bytes, count) == count;
        bytes += count;
        size -= count;
    }
    file.close();
    return ok;
}

fs::File FsStore::openCommitted(const char* target) {
    const auto base = path(target);
    return faulted_ || base.empty() ? fs::File{} : openRead(base);
}

bool FsStore::removeCommitted(const char* target) {
    const auto base = path(target);
    return !base.empty() && remove(base);
}

std::string FsStore::retainPreviousCommitted(const char* prefix, uint32_t today) {
    if (faulted_) return {};
    auto directory = openRead(root_);
    if (!directory || !directory.isDirectory()) return {};
    const size_t prefixLength = std::strlen(prefix);
    std::string previous;
    unsigned long previousDay = 0;
    std::vector<std::string> retired;
    while (auto entry = directory.openNextFile()) {
        if (entry.isDirectory()) continue;
        const std::string name = entry.name();
        if (name.size() <= prefixLength || name.compare(0, prefixLength, prefix) != 0 ||
            name.find_first_not_of("0123456789", prefixLength) != std::string::npos)
            continue;
        const unsigned long day = std::strtoul(name.c_str() + prefixLength, nullptr, 10);
        if (day >= today) continue;
        if (previous.empty() || day > previousDay) {
            if (!previous.empty()) retired.push_back(previous);
            previous = name;
            previousDay = day;
        } else {
            retired.push_back(name);
        }
    }
    directory.close();
    for (const auto& name : retired)
        if (!removeCommitted(name.c_str())) break;
    return previous;
}

void FsStore::sweep(uint32_t epochNow, bool (*validatorLive)(const PartialMeta&)) {
    if (faulted_ || epochNow < resume_detail::kValidEpoch) return;
#ifdef ARDUINO_ARCH_ESP32
    if (!fs_.mountpoint()) return;
#endif
    auto directory = openRead(root_);
    if (!directory || !directory.isDirectory()) return;
    std::vector<std::string> targets;
    while (auto entry = directory.openNextFile()) {
        if (entry.isDirectory()) continue;
        const std::string name = entry.name();
        if (hasSuffix(name, ".part") || hasSuffix(name, ".meta")) {
            targets.push_back(name.substr(0, name.size() - 5));
        }
    }
    directory.close();
    std::sort(targets.begin(), targets.end());
    targets.erase(std::unique(targets.begin(), targets.end()), targets.end());
    for (const auto& target : targets) {
        PartialMeta meta{};
        const bool paired = size(target.c_str()) > 0 && readMeta(target.c_str(), meta);
        if (faulted_) break;
        const bool stale = meta.touchedAt >= resume_detail::kValidEpoch && meta.touchedAt <= epochNow &&
                           epochNow - meta.touchedAt >= kStaleSeconds;
        if (!paired || stale || (validatorLive && !validatorLive(meta))) discard(target.c_str());
        if (faulted_) break;
    }
}

bool probePartialStorage(fs::FS& fs) {
    if (!fs.exists(kRoot) && !fs.mkdir(kRoot)) return false;
    uint8_t expected[512];
    for (size_t i = 0; i < sizeof(expected); ++i) expected[i] = static_cast<uint8_t>(i ^ (i >> 8));
    const bool written = writeSynced(fs, kProbe, expected, sizeof(expected), false);
    auto file = written ? fs.open(kProbe, "r") : fs::File{};
    uint8_t actual[512];
    const bool verified = file && file.size() == sizeof(actual) &&
                          file.read(actual, sizeof(actual)) == sizeof(actual) &&
                          std::memcmp(actual, expected, sizeof(actual)) == 0;
    file.close();
    return fs.remove(kProbe) && verified;
}
