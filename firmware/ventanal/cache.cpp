// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
#include "cache.h"
#include "logic.h"
#include "settings.h"
#include <algorithm>
#include "http.h"
#include "../common/resume/fs_store.h"
#include "../common/resume/storage.h"
#include <ctime>
#ifdef VENTANAL_HOST_TEST
#include <chrono>
#include <cstdio>
#endif
#include <esp_heap_caps.h>
#include <memory>
#include <string>
#include <WiFi.h>
#include <LittleFS.h>
#include "../lcd43b/src/miniz/ventanal_tinfl.h"

#ifndef VENTANAL_ABORT_AFTER_BYTES
#define VENTANAL_ABORT_AFTER_BYTES 0
#endif
namespace {
constexpr size_t frameSize = 960016;
constexpr uint32_t frameIdleMs = 15000;
constexpr char frameValidatorPrefix[] = "frame:";

// A zlib frame resumes on the frame identity plus its wire size, never on
// a raw partial of the same frame (or the reverse).
std::string frameValidator(const ExpectedHash& frame) {
    return frameValidatorPrefix + (frame.sha.empty() ? frame.id : frame.sha) +
        (frame.zlib ? ":zlib:" + std::to_string(frame.wireSize) : std::string{});
}

std::string frameTarget(const char* path) {
    return std::string("frame-") + (path[0] == '/' ? path + 1 : path) + ".tmp";
}

// The staged zlib wire for a cache slot: /weather.vnf -> frame-weather.vnz.tmp.
std::string wireTarget(const char* path) {
    std::string name = path[0] == '/' ? path + 1 : path;
    if (name.size() > 4 && name.compare(name.size() - 4, 4, ".vnf") == 0) name.back() = 'z';
    else name += ".z";
    return "frame-" + name + ".tmp";
}

uint32_t frameClock() { return millis(); }

auto allocateFrameChunk() {
    return std::unique_ptr<uint8_t, decltype(&heap_caps_free)>(
        static_cast<uint8_t*>(heap_caps_malloc(kResumeChunk, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
        heap_caps_free);
}
// Preserve the exact previous power-save mode on every return path.
struct DownloadPower {
    decltype(WiFi.getSleep()) previous = WiFi.getSleep();
    DownloadPower() { WiFi.setSleep(false); }
    ~DownloadPower() { WiFi.setSleep(previous); }
};
bool validate(File& f, const char* id = "", FsStore* store = nullptr, const char* sha = "") {
    FrameHash digest;
    const bool verify = *id || *sha;
    uint8_t block[512];
    const auto read = [&](size_t count) {
        return store ? store->readExact(f, block, count) : f.read(block, count) == count;
    };
    if (!f || !read(16) || !validFrameHeader(block, f.size())) return false;
    if (verify) digest.add(block, 16);
    size_t remaining = frameSize - 16;
    while (remaining) {
        const size_t n = std::min(remaining, sizeof(block));
        if (!read(n)) return false;
        for (size_t i = 0; i < n; ++i)
            if ((block[i] >> 4) > 5 || (block[i] & 15) > 5) return false;
        if (verify) digest.add(block, n);
        remaining -= n;
    }
    if (verify) {
        const auto actual = digest.finish();
        if (*sha ? strcasecmp(actual.c_str(), sha) != 0 : actual.compare(0, 16, id) != 0)
            return false;
    }
    return f.seek(16);
}
// SD stages verified bytes. Copy them into LittleFS so replacement remains
// a single rename on the same filesystem as the display cache.
bool copyToCache(FsStore& store, File& staged, const char* path, uint8_t* block, const char* sha, const char* id) {
    if (!staged.seek(0)) return false;
    const auto temporary = std::string(path) + ".tmp";
    File out = LittleFS.open(temporary.c_str(), "w");
    bool ok = bool(out);
    for (size_t remaining = frameSize; ok && remaining;) {
        const size_t count = std::min(remaining, size_t(kResumeChunk));
        ok = store.readExact(staged, block, count) && out.write(block, count) == count;
        remaining -= count;
    }
    if (out) { out.flush(); out.close(); }
    if (ok) { File check = LittleFS.open(temporary.c_str(), "r"); ok = validate(check, id, nullptr, sha); check.close(); }
    ok = ok && LittleFS.rename(temporary.c_str(), path);
    if (!ok) LittleFS.remove(temporary.c_str());
    return ok;
}

using Inflater = std::unique_ptr<tinfl_decompressor, decltype(&heap_caps_free)>;

// Inflate a whole zlib wire into out, checking the VNF1 header, every pixel
// nibble, the exact decoded size and the manifest SHA-256 on the way. The
// caller's 32 KiB download chunk is the LZ dictionary; input is read 1 KiB
// at a time. Returns nullptr or the failure reason.
const char* streamInflate(FsStore& store, File& wire, uint32_t wireSize, File& out,
                          tinfl_decompressor& inflater, uint8_t* dictionary, const char* sha) {
    static_assert(kResumeChunk == TINFL_LZ_DICT_SIZE, "the download chunk doubles as the dictionary");
    tinfl_init(&inflater);
    FrameHash digest;
    uint8_t input[1024];
    uint8_t header[16];
    size_t produced = 0, consumed = 0, inputAt = 0, inputEnd = 0, dictionaryAt = 0;
    for (;;) {
        if (inputAt == inputEnd && consumed < wireSize) {
            const size_t count = std::min(sizeof input, size_t(wireSize - consumed));
            if (!store.readExact(wire, input, count)) return "storage";
            consumed += count;
            inputAt = 0;
            inputEnd = count;
        }
        size_t inBytes = inputEnd - inputAt;
        size_t outBytes = TINFL_LZ_DICT_SIZE - dictionaryAt;
        const int flags = TINFL_FLAG_PARSE_ZLIB_HEADER | (consumed < wireSize ? TINFL_FLAG_HAS_MORE_INPUT : 0);
        const auto status = tinfl_decompress(&inflater, input + inputAt, &inBytes, dictionary,
                                             dictionary + dictionaryAt, &outBytes, flags);
        inputAt += inBytes;
        if (outBytes) {
            if (outBytes > frameSize - produced) return "oversize";
            const uint8_t* span = dictionary + dictionaryAt;
            for (size_t i = 0; i < outBytes; ++i) {
                const size_t at = produced + i;
                if (at < sizeof header) {
                    header[at] = span[i];
                    if (at + 1 == sizeof header && !validFrameHeader(header, frameSize)) return "frame";
                } else if ((span[i] >> 4) > 5 || (span[i] & 15) > 5) {
                    return "frame";
                }
            }
            digest.add(span, outBytes);
            if (out.write(span, outBytes) != outBytes) return "storage";
            produced += outBytes;
            dictionaryAt = (dictionaryAt + outBytes) & (TINFL_LZ_DICT_SIZE - 1);
        }
        if (status == TINFL_STATUS_DONE) break;
        if (status < 0) return "zlib";  // Includes a bad header or Adler-32.
        if (status == TINFL_STATUS_NEEDS_MORE_INPUT && inputAt == inputEnd && consumed == wireSize)
            return "zlib";
    }
    if (inputAt != inputEnd || consumed != wireSize) return "trailing";
    if (produced != frameSize) return "size";
    return strcasecmp(digest.finish().c_str(), sha) == 0 ? nullptr : "hash";
}

// Decode a committed wire into <path>.tmp on LittleFS and rename it over the
// cached frame only after every check passed. The wire stays committed for
// the caller to retire. Peak heap: the 32 KiB chunk plus one decompressor.
const char* inflateFrame(FsStore& store, const char* target, uint32_t wireSize, const char* sha,
                         const char* path, uint8_t* dictionary) {
    File wire = store.openCommitted(target);
    if (!wire) return "storage";
    if (wire.size() != wireSize) { wire.close(); return "size"; }
    Inflater inflater(static_cast<tinfl_decompressor*>(heap_caps_malloc(
        sizeof(tinfl_decompressor), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)), heap_caps_free);
    if (!inflater) { wire.close(); return "memory"; }
    const auto temporary = std::string(path) + ".tmp";
    File out = LittleFS.open(temporary.c_str(), "w");
    const uint32_t inflateStarted = millis();
#ifdef VENTANAL_HOST_TEST
    const auto hostInflateStarted = std::chrono::steady_clock::now();
#endif
    const char* failure = out ? streamInflate(store, wire, wireSize, out, *inflater, dictionary, sha)
                              : "storage";
#ifdef VENTANAL_HOST_TEST
    if (out) std::printf("host inflate ms=%lld bytes=%u\n", static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - hostInflateStarted).count()), unsigned(frameSize));
#endif
    if (out) Serial.printf("inflate ms=%lu bytes=%u\n", static_cast<unsigned long>(millis() - inflateStarted), unsigned(frameSize));
    wire.close();
    if (out) { out.flush(); out.close(); }
    if (!failure) { File check = LittleFS.open(temporary.c_str(), "r"); if (!validate(check, "", nullptr, sha)) failure = "stored-frame"; check.close(); }
    if (!failure && !LittleFS.rename(temporary.c_str(), path)) failure = "storage";
    if (failure) LittleFS.remove(temporary.c_str());
    return failure;
}

bool promoteFrame(FsStore& store, bool flash, File& staged, const char* target,
                  const char* path, uint8_t* block, const char* sha, const char* id) {
    if (flash) {
        staged.close();
        return store.renameCommitted(target, path);
    }
    const bool copied = copyToCache(store, staged, path, block, sha, id);
    staged.close();
    if (copied) store.removeCommitted(target);
    return copied;
}

bool committedExists(FsStore& store, const std::string& target) {
    File file = store.openCommitted(target.c_str());
    const bool found = bool(file);
    if (found) file.close();
    return found;
}

// A wire committed before power was lost is inflated now, or dropped.
bool recoverZlibFrame(FsStore& store, const char* path, const ExpectedHash& expected) {
    const auto target = wireTarget(path);
    if (!committedExists(store, target)) return false;
    const char* failure = "manifest";
    std::unique_ptr<uint8_t, decltype(&heap_caps_free)> block(nullptr, heap_caps_free);
    if (hex64(expected.sha.c_str())) {
        block = allocateFrameChunk();
        failure = block ? inflateFrame(store, target.c_str(), expected.wireSize, expected.sha.c_str(),
                                       path, block.get())
                        : "memory";
    }
    if (failure && !strcmp(failure, "memory")) return false;  // Retry on a later wake.
    store.removeCommitted(target.c_str());
    if (failure) return false;
    if (securityConfigured()) Serial.println("security frame-hash=verified");
    return true;
}

bool recoverFrame(FsStore& store, bool flash, const char* path, const ScreenInfo* frame) {
    const auto target = frameTarget(path);
    const auto* expected = frame && frame->id[0]
        ? expectedFrame((String(installation.host) + frame->url).c_str()) : nullptr;
    if (expected && expected->zlib) {
        if (committedExists(store, target)) store.removeCommitted(target.c_str());
        return recoverZlibFrame(store, path, *expected);
    }
    if (committedExists(store, wireTarget(path))) store.removeCommitted(wireTarget(path).c_str());
    auto staged = store.openCommitted(target.c_str());
    if (!staged) return false;

    if (!expected || !hex64(expected->sha.c_str()) ||
        !validate(staged, "", &store, expected->sha.c_str())) {
        staged.close();
        store.removeCommitted(target.c_str());
        if (!flash) LittleFS.remove((std::string(path) + ".tmp").c_str());
        return false;
    }
    std::unique_ptr<uint8_t, decltype(&heap_caps_free)> block(nullptr, heap_caps_free);
    if (!flash) {
        block = allocateFrameChunk();
        if (!block) return false;
    }
    if (!promoteFrame(store, flash, staged, target.c_str(), path, block.get(), expected->sha.c_str(), "")) return false;
    if (securityConfigured()) Serial.println("security frame-hash=verified");
    return true;
}
}

bool canStageExtraFrame(const char* path) {
    // On flash, bytes already checkpointed occupy part of the space reserved
    // for this frame. An SD checkpoint still needs a full LittleFS copy.
    uint64_t have = 0;
    if (partialsOnFlash()) {
        const auto size = partialStore().size(frameTarget(path).c_str());
        have = std::clamp(size, int64_t{0}, int64_t{frameSize});
    }
    return LittleFS.totalBytes() + have >= LittleFS.usedBytes() + frameSize + 65536;
}

uint8_t sweepFramePartials(const ManifestFrameSlots& frames) {
    const time_t epoch = time(nullptr);
    if (epoch >= resume_detail::kValidEpoch) {
        sweepPartials(uint32_t(epoch), [](const PartialMeta& meta) {
            if (strncmp(meta.validator, frameValidatorPrefix, sizeof(frameValidatorPrefix) - 1))
                return true;
            return std::any_of(expectedHashes.begin(), expectedHashes.end(), [&](const ExpectedHash& frame) {
                return frameValidator(frame) == meta.validator;
            });
        });
    }

    uint8_t recovered = 0;
    auto& flash = flashPartialStore();
    for (size_t i = 0; i < frames.size(); ++i) {
        // Flash can still have a committed stage from a wake without SD.
        bool promoted = recoverFrame(flash, true, frameCachePaths[i], frames[i]);
        if (!partialsOnFlash())
            promoted = recoverFrame(partialStore(), false, frameCachePaths[i], frames[i]) || promoted;
        if (promoted) recovered |= uint8_t(1u << i);
    }
    return recovered;
}

bool fetchFrame(const char* url, const char* path, uint32_t budgetMs) {
    const uint32_t started = millis();
    const auto* expected = expectedFrame(url);
    const char* sha = expected ? expected->sha.c_str() : "";
    const char* legacyId = expected && !*sha ? expected->id.c_str() : "";
    // A zlib frame downloads its wire bytes; budgets and logs count those.
    const bool zlib = expected && expected->zlib;
    const uint32_t total = zlib ? expected->wireSize : frameSize;
    const std::string target = zlib ? wireTarget(path) : frameTarget(path);
    HostRangeTransport transport(url, started, budgetMs, frameIdleMs,
                                 HostRangeTransport::Asset::Frame, VENTANAL_ABORT_AFTER_BYTES);
    auto& store = partialStore();
    const auto result = [&](FetchOutcome outcome, const char* reason) {
        const char* name = "failed";
        switch (outcome) {
            case FetchOutcome::Complete: name = "complete"; break;
            case FetchOutcome::Progress: name = "progress"; break;
            case FetchOutcome::NoProgress: name = "no-progress"; break;
            case FetchOutcome::Failed: break;
        }
        const auto have = std::max(int64_t{0}, store.size(target.c_str()));
        Serial.printf("frame slot=%s outcome=%s reason=%s status=%d got=%u have=%u total=%u ms=%lu%s\n",
                      path, name, reason, transport.status(), unsigned(transport.bytesRead()),
                      unsigned(have), unsigned(total), static_cast<unsigned long>(millis() - started),
                      zlib ? " enc=zlib" : "");
        return outcome == FetchOutcome::Complete;
    };
    // Nothing decoded is ever kept without the signed decoded-frame hash.
    if ((securityConfigured() || zlib) && !hex64(sha)) return result(FetchOutcome::Failed, "manifest");
    if (zlib && (total < 8 || total > maxZlibFrameBytes)) return result(FetchOutcome::Failed, "manifest");

    // Buffer a full checkpoint in internal RAM for flash and SD writes.
    auto block = allocateFrameChunk();
    if (!block) return result(FetchOutcome::Failed, "memory");

    std::string sourceUrl = url;
    if (sourceUrl.size() >= sizeof(PartialMeta::url)) {
        FrameHash hash;
        hash.add(url, sourceUrl.size());
        sourceUrl = "sha256:" + hash.finish();
    }
    const auto validator = expected ? frameValidator(*expected) : std::string{};
    // The wire has no hash of its own: the decoded SHA-256 is checked after inflate.
    const ResumeSource source{sourceUrl.c_str(), validator.c_str(), zlib ? "" : sha, total, true, true};
    const time_t epoch = time(nullptr);
    const char* reason = nullptr;
    const auto elapsed = millis() - started;
    if (elapsed >= budgetMs) return result(FetchOutcome::NoProgress, "budget");
    DownloadPower power;
    const bool flash = partialsOnFlash();
    const auto outcome = resumeFetch(transport, store, target.c_str(), source, budgetMs - elapsed,
        frameClock, epoch >= resume_detail::kValidEpoch ? uint32_t(epoch) : 0, logTransferOutcome,
        &reason, {}, block.get(), kResumeChunk);
    if (transport.aborted()) {
        Serial.printf("download debug-abort bytes=%u cache-preserved\n", unsigned(transport.bytesRead()));
        return result(outcome, "debug-abort");
    }
    if (&partialStore() != &store) return result(FetchOutcome::NoProgress, "storage");
    if (outcome != FetchOutcome::Complete) {
        if (!reason) {
            const int status = transport.status();
            if (transport.budgetExpired()) reason = "budget";
            else if (status > 0 && status != 200 && status != 206) reason = "http";
            else reason = "interrupted";
        }
        return result(outcome, reason);
    }

    if (zlib) {
        const char* failure = inflateFrame(store, target.c_str(), total, sha, path, block.get());
        if (failure && !strcmp(failure, "memory")) return result(FetchOutcome::NoProgress, failure);
        store.removeCommitted(target.c_str());  // Verified or not, the wire is spent.
        if (failure && &partialStore() != &store) return result(FetchOutcome::NoProgress, "storage");
        if (!failure && securityConfigured()) Serial.println("security frame-hash=verified");
        return result(failure ? FetchOutcome::Failed : FetchOutcome::Complete, failure ? failure : "ok");
    }

    auto staged = store.openCommitted(target.c_str());
    const bool valid = validate(staged, legacyId, &store);
    const bool committed = valid && promoteFrame(store, flash, staged, target.c_str(), path, block.get(), sha, legacyId);
    if (!valid) staged.close();
    if (!committed) store.removeCommitted(target.c_str());
    if (&partialStore() != &store && !committed) return result(FetchOutcome::NoProgress, "storage");
    if (committed && securityConfigured()) Serial.println("security frame-hash=verified");
    return result(committed ? FetchOutcome::Complete : FetchOutcome::Failed,
                  committed ? "ok" : valid ? "storage" : "frame");
}

bool blitFrame(const char* path, Inkplate& d) {
    File f = LittleFS.open(path, "r");
    if (!validate(f)) { f.close(); return false; }
    d.setRotation(installation.rotation);
    if (d.width() != 1600 || d.height() != 1200) { f.close(); return false; }
    uint8_t row[800];
    for (int y = 0; y < 1200; ++y) {
        if (f.read(row, sizeof(row)) != sizeof(row)) { f.close(); return false; }
        for (int x = 0; x < 1600; x += 2) {
            d.drawPixel(x, y, row[x / 2] >> 4);
            d.drawPixel(x + 1, y, row[x / 2] & 15);
        }
    }
    f.close();
    return true;
}
