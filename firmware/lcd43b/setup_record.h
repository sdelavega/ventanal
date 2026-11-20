// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
//
// What Wi-Fi setup writes and when it runs (pure; host tested).
#pragma once

#include "../common/fcc.h"
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
    size_t n = 0;
    while (n < N - 1 && src[n]) ++n;  // sources are NUL-terminated, possibly shorter arrays
    memmove(dst, src, n);  // memmove: callers may copy a field onto itself
    dst[n] = '\0';
}

// The new record: the portal's answers over the current one. The pairing
// comes from the portal (which keeps the stored one unless a new one is given)
// and is dropped only when "Leave the host" was ticked; the profile and forced
// revision are kept so a private build's seed stays write-once. The board's
// cache and screen state are untouched: the restart after saving picks the
// new mode.
inline void installationFromPortal(const Installation& current, const portal::PortalFields& f,
                                   const portal::LocationResult& where, time_t now, Installation& r) {
    if (&r != &current) r = current;
    r.schema = installationSchema;
    copyText(r.ssid, f.ssid);
    copyText(r.password, f.password);
    copyText(r.zip, f.zip);
    r.units = f.units;
    r.clock24h = f.clock24h;
    r.quietStart = f.quietStart;
    r.quietEnd = f.quietEnd;
    copyText(r.callSign, f.callSign);
    r.callState = uint8_t(r.callSign[0] ? callStateFor(lookupCallSign(r.callSign)) : CallState::None);
    r.callCheckedAt = r.callSign[0] && fccClockValid(now) ? uint32_t(now) : 0;
    if (f.leaveHost) {
        r.host[0] = r.pairing[0] = r.certificate[0] = r.deviceKey[0] = '\0';
    } else if (f.host[0]) {
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

inline bool needsSetupTimezone(const Installation& r) {
    return !r.timezone[0] || strcmp(r.timezone, "UTC0") == 0;
}

// The portal starts from the stored record (secrets are used, never shown).
inline portal::PortalFields portalFieldsFrom(const Installation& r) {
    portal::PortalFields f{};
    f.needsTimezone = needsSetupTimezone(r);
    copyText(f.ssid, r.ssid);
    copyText(f.password, r.password);
    copyText(f.zip, r.zip);
    f.units = r.units;
    f.clock24h = r.clock24h;
    f.quietStart = r.quietStart;
    f.quietEnd = r.quietEnd;
    copyText(f.callSign, r.callSign);
    copyText(f.host, r.host);
    copyText(f.pairing, r.pairing);
    copyText(f.certificate, r.certificate);
    copyText(f.deviceKey, r.deviceKey);
    return f;
}

inline Installation installationFromPortal(const Installation& current, const portal::PortalFields& f,
                                           const portal::LocationResult& where, time_t now) {
    Installation r;
    installationFromPortal(current, f, where, now, r);
    return r;
}

}  // namespace lcd
