// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
//
// Choosing the day's art on the board: the season for the date (the public
// example's table), one search term per day, and a deterministic pick from
// the candidates seeded by the date and the board, skipping pieces shown
// recently (a 64-entry ring, about 16 days of four pieces). Pure; host tested.
#pragma once

#include "art_sources.h"
#include "standalone_seasons.h"
#include "weather_data.h"
#include <cstdint>

namespace lcd::local {

inline uint32_t fnv1a(const char* text, uint32_t hash = 2166136261u) {
    for (const auto* p = reinterpret_cast<const unsigned char*>(text); *p; ++p) hash = (hash ^ *p) * 16777619u;
    return hash;
}

inline bool inSeason(const ArtSeason& s, int month, int day) {
    const int at = month * 100 + day, start = s.startMonth * 100 + s.startDay, end = s.endMonth * 100 + s.endDay;
    return start <= end ? at >= start && at <= end : at >= start || at <= end;  // a season may wrap the new year
}

inline const ArtSeason& seasonFor(const LocalDate& date) {
    for (const ArtSeason& s : ART_SEASONS)
        if (inSeason(s, date.month, date.day)) return s;
    return ART_SEASONS[0];
}

// One seed per board per day.
inline uint32_t daySeed(const LocalDate& date, const char* deviceId) {
    char key[64];
    snprintf(key, sizeof key, "%04d-%02d-%02d:%s", date.year, date.month, date.day, deviceId);
    return fnv1a(key);
}

inline const char* dayTerm(const LocalDate& date, const char* deviceId) {
    const ArtSeason& s = seasonFor(date);
    return s.terms[daySeed(date, deviceId) % s.termCount];
}

inline uint32_t candidateKey(const ArtCandidate& c) {
    return fnv1a(c.objectId, fnv1a(":", fnv1a(c.source)));
}

struct SeenRing {
    uint32_t keys[64]{};
    uint8_t next = 0, count = 0;
    bool contains(uint32_t key) const {
        for (int i = 0; i < count; ++i)
            if (keys[i] == key) return true;
        return false;
    }
    void add(uint32_t key) {
        if (contains(key)) return;
        keys[next] = key;
        next = uint8_t((next + 1) % 64);
        if (count < 64) ++count;
    }
};

// Up to `want` candidate indices in a seeded order: unseen pieces first, then
// (only if there are too few) seen ones. Returns how many were chosen.
inline int pickCandidates(const ArtCandidate* pool, int n, const SeenRing& seen, uint32_t seed, int want, int* out) {
    struct Ranked {
        uint32_t rank;
        int index;
        bool seen;
    };
    Ranked ranked[64];
    n = n < 64 ? n : 64;
    for (int i = 0; i < n; ++i) {
        const uint32_t key = candidateKey(pool[i]);
        ranked[i] = {(key ^ seed) * 2654435761u, i, seen.contains(key)};
    }
    for (int i = 1; i < n; ++i)  // insertion sort: unseen first, then by rank
        for (int j = i; j > 0; --j) {
            const Ranked& a = ranked[j - 1];
            const Ranked& b = ranked[j];
            if (a.seen > b.seen || (a.seen == b.seen && a.rank > b.rank)) {
                const Ranked t = ranked[j - 1];
                ranked[j - 1] = ranked[j];
                ranked[j] = t;
            } else {
                break;
            }
        }
    const int count = want < n ? want : n;
    for (int i = 0; i < count; ++i) out[i] = ranked[i].index;
    return count;
}

}  // namespace lcd::local
