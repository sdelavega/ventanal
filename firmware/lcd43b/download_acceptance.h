// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
#pragma once

#include "ota.h"
#include "scanout_diag.h"
#include "../common/release_info.h"
#include <Preferences.h>

namespace lcd {

struct AcceptedUpdate {
    UpdateSource source = UpdateSource::None;
    UpdateOffer offer{};
    ReleaseInfo image{};
};

inline constexpr char acceptedUpdateKey[] = "ota-accept1";

inline AcceptedUpdate loadAcceptedUpdate(Preferences& prefs) {
    AcceptedUpdate accepted{};
    if (prefs.getBytesLength(acceptedUpdateKey) != sizeof accepted ||
        prefs.getBytes(acceptedUpdateKey, &accepted, sizeof accepted) != sizeof accepted)
        return {};
    return accepted;
}

inline bool saveAcceptedUpdate(Preferences& prefs, const AcceptedUpdate& accepted) {
    ScanScope writing(ScanActivity::Nvs);
    return prefs.putBytes(acceptedUpdateKey, &accepted, sizeof accepted) == sizeof accepted;
}

inline bool clearAcceptedUpdate(Preferences& prefs, AcceptedUpdate& accepted) {
    ScanScope writing(ScanActivity::Nvs);
    if (prefs.isKey(acceptedUpdateKey) && !prefs.remove(acceptedUpdateKey)) return false;
    accepted = {};
    return true;
}

inline bool matchesAcceptance(const AcceptedUpdate& left, const AcceptedUpdate& right) {
    return left.source == right.source &&
        !std::strcmp(left.offer.version, right.offer.version) &&
        !std::strcmp(left.offer.signedOffer.approval, right.offer.signedOffer.approval) &&
        !std::strcmp(left.offer.signedOffer.path, right.offer.signedOffer.path) &&
        releaseImageMatches(left.image, right.image.size, right.image.sha256);
}

inline bool reconcileAcceptedUpdate(Preferences& prefs, AcceptedUpdate& accepted,
                                    const AcceptedUpdate& current) {
    return accepted.source == UpdateSource::None || matchesAcceptance(accepted, current) ||
        clearAcceptedUpdate(prefs, accepted);
}

AcceptedUpdate identifyUpdate(const UpdateOffer& offer, UpdateSource source);

}
