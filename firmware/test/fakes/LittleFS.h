// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
#pragma once
#include "Arduino.h"
#include <map>
#include <vector>
#include <cstring>
inline std::map<std::string, std::vector<uint8_t>> files;
inline bool failRename = false;
inline bool failWrite = false;
inline size_t writeCalls = 0;
inline size_t largestWrite = 0;
class File {
    std::string path; size_t pos = 0; bool opened = false;
public:
    File() = default;
    File(std::string p, bool ok):path(p),opened(ok) {}
    explicit operator bool() const { return opened; }
    size_t size() const { return files[path].size(); }
    size_t read(uint8_t* b, size_t n) {
        n = std::min(n, size()-pos); std::memcpy(b, files[path].data()+pos, n); pos += n; return n;
    }
    size_t write(const uint8_t* b, size_t n) { if (failWrite) return 0; ++writeCalls; largestWrite=std::max(largestWrite,n); files[path].insert(files[path].end(), b, b+n); return n; }
    bool seek(size_t n) { pos = n; return n <= size(); }
    void flush() {}
    void close() { opened = false; }
};
struct FakeFS {
    size_t capacity = 0x1BD0000;
    size_t totalBytes() const { return capacity; }
    size_t usedBytes() const { size_t n=0; for(const auto& f:files) n+=f.second.size(); return n; }
    bool failBegin = false;
    bool begin(bool,const char*,int,const char*) { return !failBegin; }
    void end() {}
    bool exists(const char* p) { return files.count(p); }
    bool exists(const String& p) { return exists(p.c_str()); }
    File open(const char* p, const char* mode) { bool ok = files.count(p); if (*mode=='w') { files[p].clear(); ok=true; } return File(p, ok); }
    File open(const String& p, const char* mode) { return open(p.c_str(), mode); }
    bool remove(const char* p) { return files.erase(p); }
    bool remove(const String& p) { return remove(p.c_str()); }
    bool rename(const char* a, const char* b) { if(failRename) return false; files[b]=files[a]; files.erase(a); return true; }
    bool rename(const String& a, const String& b) { return rename(a.c_str(), b.c_str()); }
};
inline FakeFS LittleFS;
