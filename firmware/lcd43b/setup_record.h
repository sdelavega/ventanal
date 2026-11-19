// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
//
// What Wi-Fi setup writes and when it runs (pure; host tested).
#pragma once

#include "../common/setup_portal/portal.h"
#include "installation.h"
#include <cstring>

namespace lcd {

enum class SetupReason { FirstBoot, UserRequested };
enum class BootSetup { None, FirstBoot };
enum class SetupNext { Stay, Restart, Return, Reopen };

// A board that cannot run from its stored record opens setup at boot: no
// record at all, or (until standalone mode lands) a standalone record.
inline BootSetup bootSetup(bool stored, bool ready) {
    (void)stored;  // both unusable cases need the same thing: a way to set it up
    return ready ? BootSetup::None : BootSetup::FirstBoot;
}

inline SetupNext afterSetup(portal::PortalOutcome outcome, SetupReason why) {
    switch (outcome) {
    case portal::PortalOutcome::Saved: return SetupNext::Restart;
    case portal::PortalOutcome::Cancelled: return SetupNext::Return;
    case portal::PortalOutcome::TimedOut:
        return why == SetupReason::FirstBoot ? SetupNext::Reopen : SetupNext::Return;
    case portal::PortalOutcome::Running: break;
    }
    return SetupNext::Stay;
}

// A board with nothing usable to go back to has no Cancel.
inline bool setupCancelVisible(SetupReason why) { return why == SetupReason::UserRequested; }

template<size_t N>
inline void copyText(char (&dst)[N], const char* src) {
    strncpy(dst, src, N - 1);
    dst[N - 1] = '\0';
}

// The new record: the portal's answers over the current one. Host pairing
// is kept unless the portal supplied a new one (leaving a host is explicit,
// in a later task), and the profile and forced revision are kept so a
// private build's seed stays write-once.
inline void installationFromPortal(const Installation& current, const portal::PortalFields& f,
                                   const portal::LocationResult& where, Installation& r) {
    if (&r != &current) r = current;
    r.schema = installationSchema;
    copyText(r.ssid, f.ssid);
    copyText(r.password, f.password);
    copyText(r.zip, f.zip);
    r.units = f.units;
    r.clock24h = f.clock24h;
    r.quietStart = f.quietStart;
    r.quietEnd = f.quietEnd;
    if (strcmp(r.callSign, f.callSign)) {
        copyText(r.callSign, f.callSign);
        r.callState = uint8_t(CallState::None);
        r.callCheckedAt = 0;
    }
    if (f.host[0]) {
        copyText(r.host, f.host);
        copyText(r.pairing, f.pairing);
        copyText(r.certificate, f.certificate);
        copyText(r.deviceKey, f.deviceKey);
    }
    if (where.ok) {
        copyText(r.timezone, where.posixTz);
        copyText(r.place, where.place);
        r.latitude = where.latitude;
        r.longitude = where.longitude;
    }
}

inline Installation installationFromPortal(const Installation& current, const portal::PortalFields& f,
                                           const portal::LocationResult& where) {
    Installation r;
    installationFromPortal(current, f, where, r);
    return r;
}

}  // namespace lcd
