// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
#pragma once

#include "Arduino.h"
#include <cerrno>
#include <cstring>
#include <ctime>
#include <functional>
#include <map>
#include <set>
#include <vector>

inline bool failRename = false;
inline std::string truncateOnClose;
inline bool failWrite = false;
inline size_t writeLimit = SIZE_MAX;
inline size_t writeCalls = 0;
inline size_t largestWrite = 0;
inline size_t readCalls = 0;

namespace fs {
using Files = std::map<std::string, std::vector<uint8_t>>;

class File {
    Files* files_ = nullptr;
    std::string filename_;
    size_t pos_ = 0;
    bool directory_ = false;
    std::string lastEntry_;

public:
    time_t modified = 0;
    std::function<void()> beforeWrite;
    time_t getLastWrite() const { return modified; }
    std::map<std::string, time_t>* timestamps = nullptr;
    File() = default;
    File(Files& files, const char* path, bool directory = false)
        : files_(&files), filename_(path), directory_(directory) {}

    explicit operator bool() const { return files_ != nullptr; }
    const char* path() const { return filename_.c_str(); }
    const char* name() const {
        const char* slash = std::strrchr(path(), '/');
        return slash ? slash + 1 : path();
    }
    bool isDirectory() const { return files_ && directory_; }

    File openNextFile() {
        if (!isDirectory()) return {};
        const auto prefix = filename_ == "/" ? filename_ : filename_ + "/";
        auto entry = lastEntry_.empty() ? files_->lower_bound(prefix) : files_->upper_bound(lastEntry_);
        for (; entry != files_->end() && entry->first.compare(0, prefix.size(), prefix) == 0; ++entry) {
            if (entry->first.find('/', prefix.size()) != std::string::npos) continue;
            lastEntry_ = entry->first;
            File file(*files_, lastEntry_.c_str());
            if (timestamps && timestamps->count(lastEntry_)) file.modified = timestamps->at(lastEntry_);
            return file;
        }
        return {};
    }

    size_t size() const { return files_->at(filename_).size(); }
    size_t read(uint8_t* bytes, size_t count) {
        ++readCalls;
        count = std::min(count, size() - pos_);
        if (count) std::memcpy(bytes, files_->at(filename_).data() + pos_, count);
        pos_ += count;
        return count;
    }
    size_t write(const uint8_t* bytes, size_t count) {
        if (beforeWrite) beforeWrite();
        if (failWrite) return 0;
        ++writeCalls;
        count = std::min(count, writeLimit);
        largestWrite = std::max(largestWrite, count);
        auto& data = files_->at(filename_);
        if (count) data.insert(data.end(), bytes, bytes + count);
        return count;
    }
    bool seek(size_t pos) { pos_ = pos; return pos <= size(); }
    void flush() {}
    void close() { if (files_ && filename_ == truncateOnClose) { files_->at(filename_).resize(16); truncateOnClose.clear(); } files_ = nullptr; }
};

class FS {
public:
    Files files;
    std::map<std::string, time_t> modified;
    std::function<void()> beforeWrite;
    std::set<std::string> directories{"/"};
    bool offline = false;
    bool replaceOnRename = true;

    bool exists(const char* path) { return bool(open(path)); }
    bool exists(const String& path) { return exists(path.c_str()); }
    File open(const char* path, const char* mode = "r") {
        if (offline) { errno = EIO; return {}; }
        if (directories.count(path)) {
            File dir(files, path, true);
            dir.timestamps = &modified;
            return dir;
        }
        if (*mode == 'w') files[path].clear();
        if (*mode == 'a') files.try_emplace(path);
        if (files.count(path)) {
            File file(files, path);
            file.beforeWrite = beforeWrite;
            return file;
        }
        errno = ENOENT;
        return {};
    }
    File open(const String& path, const char* mode) { return open(path.c_str(), mode); }
    bool mkdir(const char* path) { return !offline && directories.insert(path).second; }
    bool remove(const char* path) {
        if (offline) { errno = EIO; return false; }
        if (files.erase(path)) return true;
        errno = ENOENT;
        return false;
    }
    bool remove(const String& path) { return remove(path.c_str()); }
    bool rename(const char* from, const char* to) {
        if (offline || failRename) { errno = EIO; return false; }
        if (!replaceOnRename && files.count(to)) { errno = EEXIST; return false; }
        auto entry = files.find(from);
        if (entry == files.end()) { errno = ENOENT; return false; }
        files[to] = std::move(entry->second);
        files.erase(entry);
        return true;
    }
    bool rename(const String& from, const String& to) { return rename(from.c_str(), to.c_str()); }
    bool truncate(const char* path, uint32_t size) {
        if (offline || !files.count(path)) return false;
        files.at(path).resize(size);
        return true;
    }
};
}

using fs::File;
