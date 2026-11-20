// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
#pragma once

#include "../common/resume/fs_store.h"
#include "../common/resume/resume.h"
#include <string_view>

namespace lcd {

class SweepDay {
public:
    bool due(uint32_t now) {
        if (now < resume_detail::kValidEpoch || now / 86400 <= lastDay_) return false;
        lastDay_ = now / 86400;
        return true;
    }
private:
    uint32_t lastDay_ = 0;
};

struct DownloadSweep {
    std::vector<std::string> art;
    std::string otaTarget, otaValidator;
    std::function<bool(FsStore&, const char*)> otaCommittedMatches;
    // With no verified host manifest yet, leave frame stages for the first check-in.
    FsStore::ValidatorLive frameLive;
    std::function<bool(FsStore&, const char*)> frameCommittedWanted;

    void run(FsStore& store, uint32_t now, bool daily = true) const {
        if (now < resume_detail::kValidEpoch) return;
        const auto day = now / 86400;
        const auto managed = [&](const char* target) {
            return (daily && (sunDay(target) || isArt(target) || isOta(target))) ||
                (frameLive && !std::strncmp(target, "frame-", 6));
        };
        const auto artLive = [&](const char* target) {
            return std::find(art.begin(), art.end(), target) != art.end();
        };
        const auto entries = store.committedEntries();
        uint32_t previous = 0;
        if (daily) {
            for (const auto& entry : entries) {
                const auto candidate = sunDay(entry.target.c_str());
                if (candidate < day) previous = std::max(previous, candidate);
            }
        }
        store.sweep(now, [&](const char* target, const PartialMeta& meta) {
            if (const auto sun = sunDay(target)) return sun == day;
            if (isArt(target)) return artLive(target);
            if (isOta(target)) return otaTarget == target && otaValidator == meta.validator;
            return frameLive(target, meta);
        }, managed);

        for (const auto& entry : entries) {
            const char* target = entry.target.c_str();
            const auto sun = sunDay(target);
            if (frameCommittedWanted && !std::strncmp(target, "frame-", 6) && !frameCommittedWanted(store, target)) {
                store.removeCommitted(target);
            } else if (daily && ((sun && sun != day && sun != previous) ||
                (isArt(target) && !artLive(target)) ||
                (isOta(target) && (entry.target != otaTarget ||
                    (entry.modified >= resume_detail::kValidEpoch && entry.modified <= now &&
                        now - entry.modified >= 7 * 86400) ||
                    !otaCommittedMatches || !otaCommittedMatches(store, target)))))
                store.removeCommitted(target);
        }
    }

private:
    static uint32_t sunDay(const char* target) {
        if (std::strncmp(target, "sdo-", 4)) return 0;
        uint32_t day = 0;
        for (const char* p = target + 4; *p; ++p) {
            if (*p < '0' || *p > '9' || day > UINT32_MAX / 10 - 1) return 0;
            day = day * 10 + uint32_t(*p - '0');
        }
        return day;
    }
    static bool isArt(const char* target) {
        if (std::strncmp(target, "art-", 4)) return false;
        const char* object = std::strchr(target + 4, '-');
        if (!object || !object[1]) return false;
        const std::string_view source(target + 4, object - (target + 4));
        return source == "aic" || source == "cleveland" || source == "met";
    }
    static bool isOta(const char* target) {
        if (std::strncmp(target, "ota-", 4)) return false;
        const char* p = target + 4;
        for (int field = 0; field < 3; ++field) {
            const char* start = p;
            while (*p >= '0' && *p <= '9') ++p;
            if (p == start || *p != (field < 2 ? '.' : '\0')) return false;
            if (field < 2) ++p;
        }
        return true;
    }
};
}
