// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
#include "../common/fcc.h"
#include <cassert>
#include <cstdio>

static void lookup_is_unavailable_and_trusted() {
    const auto result = lookupCallSign("W1AW");
    assert(result == FccResult::Unavailable);
    assert(callStateFor(result) == CallState::Unverified);
}

static void call_state_mapping() {
    assert(callStateFor(FccResult::Verified) == CallState::Verified);
    assert(callStateFor(FccResult::NotFound) == CallState::Rejected);
    assert(callStateFor(FccResult::Unavailable) == CallState::Unverified);
}

static void fcc_recheck_needs_valid_time() {
    constexpr uint32_t last = 1704067200;
    constexpr time_t day = 86400;
    assert(!fccRecheckDue(0, 1000));
    assert(!fccRecheckDue(0, last - 1));
    assert(fccRecheckDue(0, last));
    assert(!fccRecheckDue(last, last + 6 * day));
    assert(!fccRecheckDue(last, last + 7 * day - 1));
    assert(fccRecheckDue(last, last + 7 * day));
    assert(!fccRecheckDue(last + day, last));
}

int main() {
    lookup_is_unavailable_and_trusted();
    call_state_mapping();
    fcc_recheck_needs_valid_time();
    puts("FCC: trust fallback, call state mapping, and recheck timing passed");
}
