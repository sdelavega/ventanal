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

class WriteScope {
public:
    explicit WriteScope(void (*hook)(bool)) : hook_(hook) { if (hook_) hook_(true); }
    ~WriteScope() { if (hook_) hook_(false); }
private:
    void (*hook_)(bool);
};

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
        hasSuffix(name, ".part") || hasSuffix(name, ".meta") ||
        hasSuffix(name, ".fail") || hasSuffix(name, ".temp")) return {};
    return root_ + '/' + name;
}

bool FsStore::fail() {
    if (ioError_ && !faulted_.exchange(true)) ioError_();
    return false;
}

bool FsStore::ensureRoot() {
    if (fs_.exists(root_.c_str())) return true;
    WriteScope scope(writeHook_);
    return fs_.mkdir(root_.c_str()) || fail();
}

fs::File FsStore::openRead(const std::string& path) {
    errno = 0;
    auto file = fs_.open(path.c_str(), "r");
    if (!file && errno != ENOENT) fail();
    return file;
}

bool FsStore::remove(const std::string& path) {
    WriteScope scope(writeHook_);
    errno = 0;
    return fs_.remove(path.c_str()) || errno == ENOENT || fail();
}

bool FsStore::rename(const std::string& from, const std::string& to) {
    WriteScope scope(writeHook_);
    return fs_.rename(from.c_str(), to.c_str());
}

bool FsStore::write(const std::string& path, const uint8_t* bytes, size_t size, bool append) {
    WriteScope scope(writeHook_);
    return writeSynced(fs_, path, bytes, size, append);
}

bool FsStore::readMeta(const char* target, PartialMeta& meta) {
    const auto base = path(target);
    if (faulted_ || base.empty()) return false;
    auto file = openRead(base + ".meta");
    if (!file && !ioError_) file = openRead(base + ".fail");
    if (!file) return false;
    const auto count = file.size();
    if (count != kPartialMetaV1Bytes && count != kPartialMetaBytes) return false;
    uint8_t bytes[kPartialMetaBytes];
    if (file.read(bytes, count) != count) return fail();
    return decodePartialMeta(bytes, count, meta);
}

bool FsStore::writeMeta(const char* target, const PartialMeta& meta) {
    const auto base = path(target);
    if (faulted_ || base.empty() || !ensureRoot()) return false;
    const auto bytes = encodePartialMeta(meta);
    if (ioError_)
        return write(base + ".meta", bytes.data(), bytes.size(), false) || fail();

    // Publish atomically so ENOSPC cannot damage the previous checkpoint.
    const auto temporary = base + ".temp";
    if (!write(temporary, bytes.data(), bytes.size(), false) ||
        !rename(temporary, base + ".meta")) {
        remove(temporary);
        return false;
    }
    clearWriteFailure(base);
    {
        const std::lock_guard<std::mutex> lock(checkpointMutex_);
        emptyStrikes_.erase(base);
    }
    return true;
}

bool FsStore::append(const char* target, const uint8_t* bytes, size_t count) {
    const auto base = path(target);
    if (faulted_ || base.empty() || !ensureRoot()) return false;
    return write(base + ".part", bytes, count, true) || fail();
}

bool FsStore::keepAfterWriteFailure(const char* target, uint32_t have) {
    const auto base = path(target);
    // The removable store is identified by its I/O fallback handler.
    if (ioError_ || base.empty()) return false;
    const std::lock_guard<std::mutex> lock(checkpointMutex_);
    const auto marker = base + ".fail";
    const auto sidecar = base + ".meta";
    if (emptyStrikes_.count(base) || failedCheckpoints_.count(base) || (!fs_.exists(sidecar.c_str()) && fs_.exists(marker.c_str())))
        return false;

    // A failed append may have written a tail. If truncation also fails, the
    // sidecar still fences the tail out of the next download.
    if (size(target) > have) truncate(target, have);
    if (!have) emptyStrikes_.insert(base);
    else if (!rename(sidecar, marker)) failedCheckpoints_.insert(base);
    return true;
}

void FsStore::clearWriteFailure(const std::string& base) {
    if (ioError_) return;
    const std::lock_guard<std::mutex> lock(checkpointMutex_);
    remove(base + ".fail");
    failedCheckpoints_.erase(base);
}

bool FsStore::truncate(const char* target, uint32_t length) {
    const int64_t current = size(target);
    if (current < 0 || current < length) return false;
    if (current == length) return true;
    const auto part = path(target) + ".part";
    WriteScope scope(writeHook_);
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
    if (!rename(part, base)) {
        if (errno != EEXIST) return fail();
        // FAT requires removing the old file first. The final path is absent
        // between removal and rename, and stays absent if the retry fails.
        if (!remove(base)) return false;
        if (!rename(part, base)) return fail();
    }
    remove(base + ".meta");
    clearWriteFailure(base);
    return true;
}

void FsStore::discard(const char* target) {
    const auto base = path(target);
    if (base.empty()) return;
    {
        const std::lock_guard<std::mutex> lock(checkpointMutex_);
        emptyStrikes_.erase(base);
    }
    discardFiles(base);
}

void FsStore::resetPartial(const char* target) {
    const auto base = path(target);
    if (!base.empty()) discardFiles(base);
}

void FsStore::discardFiles(const std::string& base) {
    remove(base + ".meta");
    clearWriteFailure(base);
    if (!ioError_) remove(base + ".temp");
    remove(base + ".part");
}

bool FsStore::sha256(const char* target, uint8_t out[32], const HashFeed& feed) {
    const auto base = path(target);
    if (faulted_ || base.empty()) return false;
    auto file = openRead(base + ".part");
    if (!file) return false;
    Sha256 hash;
    uint8_t buffer[kPartialHashBlockBytes];
    size_t remaining = file.size();
    while (remaining) {
        if (feed) feed();
        const size_t count = std::min(remaining, sizeof(buffer));
        if (!readExact(file, buffer, count)) return false;
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

bool FsStore::renameCommitted(const char* target, const char* destination) {
    const auto base = path(target);
    return !faulted_ && !base.empty() && (rename(base, destination) || fail());
}

bool FsStore::moveCommitted(const char* from, const char* to) {
    const auto a = path(from), b = path(to);
    return !faulted_ && !a.empty() && !b.empty() && (rename(a, b) || fail());
}

bool FsStore::seek(fs::File& file, size_t position) {
    return file.seek(position) || fail();
}

bool FsStore::readExact(fs::File& file, uint8_t* bytes, size_t count) {
    return file.read(bytes, count) == count || fail();
}

void FsStore::retainOtaTarget(const char* target) {
    if (faulted_) return;
    auto directory = openRead(root_);
    if (!directory || !directory.isDirectory()) return;
    std::vector<std::string> retired;
    while (auto entry = directory.openNextFile()) {
        if (entry.isDirectory()) continue;
        const std::string name = entry.name();
        if (name.compare(0, 4, "ota-") != 0) continue;
        auto base = name;
        if (hasSuffix(name, ".part") || hasSuffix(name, ".meta") ||
            hasSuffix(name, ".fail") || hasSuffix(name, ".temp")) base.resize(name.size() - 5);
        if (base != target && !path(base.c_str()).empty()) {
            retired.push_back(name);
            const std::lock_guard<std::mutex> lock(checkpointMutex_);
            failedCheckpoints_.erase(path(base.c_str()));
            emptyStrikes_.erase(path(base.c_str()));
        }
    }
    directory.close();
    for (const auto& name : retired)
        if (!remove(root_ + '/' + name)) break;
}

bool FsStore::removeCommitted(const char* target) {
    const auto base = path(target);
    return !base.empty() && remove(base);
}

std::string FsStore::previousCommitted(const char* prefix, uint32_t today) {
    const size_t prefixLength = std::strlen(prefix);
    std::string previous;
    unsigned long previousDay = 0;
    for (const auto& entry : committedEntries()) {
        const auto& name = entry.target;
        if (name.size() <= prefixLength || name.compare(0, prefixLength, prefix) != 0 ||
            name.find_first_not_of("0123456789", prefixLength) != std::string::npos)
            continue;
        const auto day = std::strtoul(name.c_str() + prefixLength, nullptr, 10);
        if (day >= today || day <= previousDay) continue;
        previous = name;
        previousDay = day;
    }
    return previous;
}

std::vector<FsStore::CommittedEntry> FsStore::committedEntries() {
    std::vector<CommittedEntry> entries;
    if (faulted_) return entries;
#ifdef ARDUINO_ARCH_ESP32
    if (!fs_.mountpoint()) return entries;
#endif
    auto directory = openRead(root_);
    if (!directory || !directory.isDirectory()) return entries;
    while (auto entry = directory.openNextFile()) {
        if (entry.isDirectory() || path(entry.name()).empty()) continue;
        const auto modified = entry.getLastWrite();
        entries.push_back({entry.name(), modified > 0 ? uint32_t(modified) : 0});
    }
    return entries;
}

void FsStore::sweep(uint32_t epochNow, bool (*validatorLive)(const PartialMeta&)) {
    sweep(epochNow, [=](const char*, const PartialMeta& meta) {
        return !validatorLive || validatorLive(meta);
    }, {});
}

void FsStore::sweep(uint32_t epochNow, const ValidatorLive& validatorLive, const TargetFilter& managed) {
    if (faulted_ || epochNow < resume_detail::kValidEpoch) return;
#ifdef ARDUINO_ARCH_ESP32
    if (!fs_.mountpoint()) return;
#endif
    auto directory = openRead(root_);
    if (!directory || !directory.isDirectory()) return;
    std::vector<std::string> targets;
    std::vector<std::string> temporaries;
    while (auto entry = directory.openNextFile()) {
        if (entry.isDirectory()) continue;
        const std::string name = entry.name();
        const bool checkpoint = hasSuffix(name, ".part") || hasSuffix(name, ".meta");
        const bool failed = hasSuffix(name, ".fail");
        const bool temporary = hasSuffix(name, ".temp");
        // Strip suffixes for the managed filter only on checkpoints and their housekeeping files.
        // Committed names already identify the full target.
        const auto target = checkpoint || failed || temporary ? name.substr(0, name.size() - 5) : name;
        if (managed && !managed(target.c_str())) continue;
        if (!ioError_ && temporary) temporaries.push_back(name);
        if (checkpoint || (!ioError_ && failed)) targets.push_back(target);
    }
    directory.close();
    for (const auto& name : temporaries) remove(root_ + '/' + name);
    std::sort(targets.begin(), targets.end());
    targets.erase(std::unique(targets.begin(), targets.end()), targets.end());
    for (const auto& target : targets) {
        PartialMeta meta{};
        const bool paired = size(target.c_str()) > 0 && readMeta(target.c_str(), meta);
        if (faulted_) break;
        const bool stale = meta.touchedAt >= resume_detail::kValidEpoch && meta.touchedAt <= epochNow &&
                           epochNow - meta.touchedAt >= kStaleSeconds;
        if (!paired || stale || (validatorLive && !validatorLive(target.c_str(), meta))) discard(target.c_str());
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
