// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
#include "cache.h"
#include "logic.h"
#include "settings.h"
#include <algorithm>
#include "http.h"
#include "../common/resume/fs_store.h"
#include "../common/resume/storage.h"
#include <cstdarg>
#include <ctime>
#include <esp_heap_caps.h>
#include <memory>
#include <WiFi.h>
#include <LittleFS.h>

#ifndef VENTANAL_ABORT_AFTER_BYTES
#define VENTANAL_ABORT_AFTER_BYTES 0
#endif
namespace {
constexpr size_t frameSize = 960016;
constexpr uint32_t frameBudgetMs = 30000;
constexpr uint32_t frameIdleMs = 15000;
constexpr char frameValidatorPrefix[] = "frame:";

std::string frameValidator(const ExpectedHash& frame) {
    return frameValidatorPrefix + (frame.sha.empty() ? frame.id : frame.sha);
}

std::string frameTarget(const char* path) {
    return std::string("frame-") + (path[0] == '/' ? path + 1 : path) + ".tmp";
}

uint32_t frameClock() { return millis(); }

void logResume(const char* format, ...) {
    char line[128];
    va_list args;
    va_start(args, format);
    vsnprintf(line, sizeof line, format, args);
    va_end(args);
    Serial.println(line);
}
// Preserve the exact previous power-save mode on every return path.
struct DownloadPower {
    decltype(WiFi.getSleep()) previous = WiFi.getSleep();
    DownloadPower() { WiFi.setSleep(false); }
    ~DownloadPower() { WiFi.setSleep(previous); }
};
bool validate(File& f, const char* id = "", FsStore* store = nullptr) {
    FrameHash digest;
    uint8_t block[512];
    const auto read = [&](size_t count) {
        return store ? store->readExact(f, block, count) : f.read(block, count) == count;
    };
    if (!f || !read(16) || !validFrameHeader(block, f.size())) return false;
    if (*id) digest.add(block, 16);
    size_t remaining = frameSize - 16;
    while (remaining) {
        const size_t n = std::min(remaining, sizeof(block));
        if (!read(n)) return false;
        for (size_t i = 0; i < n; ++i)
            if ((block[i] >> 4) > 5 || (block[i] & 15) > 5) return false;
        if (*id) digest.add(block, n);
        remaining -= n;
    }
    return (!*id || digest.finish().compare(0, 16, id) == 0) && f.seek(16);
}
// SD stages verified bytes. Copy them into LittleFS so replacement remains
// a single rename on the same filesystem as the display cache.
bool copyToCache(FsStore& store, File& staged, const char* path, uint8_t* block) {
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
    ok = ok && LittleFS.rename(temporary.c_str(), path);
    if (!ok) LittleFS.remove(temporary.c_str());
    return ok;
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

void sweepFramePartials() {
    const time_t epoch = time(nullptr);
    if (epoch < resume_detail::kValidEpoch) return;
    sweepPartials(uint32_t(epoch), [](const PartialMeta& meta) {
        if (strncmp(meta.validator, frameValidatorPrefix, sizeof(frameValidatorPrefix) - 1))
            return true;
        return std::any_of(expectedHashes.begin(), expectedHashes.end(), [&](const ExpectedHash& frame) {
            return frameValidator(frame) == meta.validator;
        });
    });
}

bool fetchFrame(const char* url, const char* path) {
    const uint32_t started = millis();
    const auto* expected = expectedFrame(url);
    const char* sha = expected ? expected->sha.c_str() : "";
    const char* legacyId = expected && !*sha ? expected->id.c_str() : "";
    const std::string target = frameTarget(path);
    HostRangeTransport transport(url, started, frameBudgetMs, frameIdleMs,
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
        Serial.printf("frame slot=%s outcome=%s reason=%s status=%d got=%u have=%u total=%u ms=%lu\n",
                      path, name, reason, transport.status(), unsigned(transport.bytesRead()),
                      unsigned(have), unsigned(frameSize), static_cast<unsigned long>(millis() - started));
        return outcome == FetchOutcome::Complete;
    };
    if (securityConfigured() && !hex64(sha)) return result(FetchOutcome::Failed, "manifest");

    // Buffer a full checkpoint in internal RAM for flash and SD writes.
    std::unique_ptr<uint8_t, decltype(&heap_caps_free)> block(
        static_cast<uint8_t*>(heap_caps_malloc(kResumeChunk, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
        heap_caps_free);
    if (!block) return result(FetchOutcome::Failed, "memory");

    std::string sourceUrl = url;
    if (sourceUrl.size() >= sizeof(PartialMeta::url)) {
        FrameHash hash;
        hash.add(url, sourceUrl.size());
        sourceUrl = "sha256:" + hash.finish();
    }
    const auto validator = expected ? frameValidator(*expected) : std::string{};
    const ResumeSource source{sourceUrl.c_str(), validator.c_str(), sha, frameSize, true, true};
    const time_t epoch = time(nullptr);
    const char* reason = nullptr;
    const auto elapsed = millis() - started;
    if (elapsed >= frameBudgetMs) return result(FetchOutcome::NoProgress, "budget");
    DownloadPower power;
    const bool flash = partialsOnFlash();
    const auto outcome = resumeFetch(transport, store, target.c_str(), source, frameBudgetMs - elapsed,
        frameClock, epoch >= resume_detail::kValidEpoch ? uint32_t(epoch) : 0, logResume, &reason,
        {}, block.get());
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

    auto staged = store.openCommitted(target.c_str());
    const bool valid = validate(staged, legacyId, &store);
    bool committed = false;
    if (valid && !flash) committed = copyToCache(store, staged, path, block.get());
    staged.close();
    if (valid && flash) committed = store.renameCommitted(target.c_str(), path);
    if (!flash || !committed) store.removeCommitted(target.c_str());
    if (&partialStore() != &store && !committed) return result(FetchOutcome::NoProgress, "storage");
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
