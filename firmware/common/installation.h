// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
#pragma once

#include <Preferences.h>
#include <cstddef>
#include <cstdint>

// A board's record begins with its schema and carries the last forced revision.
// A migration transforms the old bytes in place and must set the new schema.
template<class Settings>
bool loadInstallationSettings(Settings& out, const char* name, const char* key,
                              const Settings* seed, uint32_t forceRevision, bool persistMigration,
                              bool (*valid)(const Settings&), bool (*migrate)(Settings&, size_t)) {
    out = {};
    const auto currentSchema = out.schema;
    const bool canSeed = seed && valid(*seed);
    Preferences prefs;
    if (!prefs.begin(name, !canSeed)) return false;
    const size_t length = prefs.getBytesLength(key);
    const bool understoodLength = length >= sizeof out.schema && length <= sizeof out;
    if (understoodLength && prefs.getBytes(key, &out, length) != length) {
        out = {};
        return false; // An I/O failure is not evidence that the stored blob is corrupt.
    }
    // Preserve data written by a newer build, including records larger than ours.
    if (length > sizeof out || (understoodLength && out.schema > currentSchema)) {
        out = {};
        return false;
    }
    const bool wasCurrent = length == sizeof out && out.schema == currentSchema;
    const bool migrated = understoodLength && out.schema < currentSchema && migrate(out, length) && valid(out);
    const bool usable = migrated || (wasCurrent && valid(out));
    if (canSeed && (!usable || (forceRevision && out.forcedRevision != forceRevision))) {
        auto replacement = *seed;
        replacement.forcedRevision = forceRevision;
        if (prefs.putBytes(key, &replacement, sizeof replacement) == sizeof replacement) {
            out = replacement;
            return true;
        }
    } else if (migrated && persistMigration) {
        if (!canSeed) {
            prefs.end();
            if (!prefs.begin(name, false)) return true; // Retry persistence on the next wake.
        }
        prefs.putBytes(key, &out, sizeof out);
    }
    if (!usable) out = {};
    return usable;
}
