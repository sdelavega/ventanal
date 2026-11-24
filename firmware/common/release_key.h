// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
#pragma once

#include <stdint.h>
#include "hardening.h"

// Active key. Install together with the server release public key.
constexpr uint8_t VENTANAL_RELEASE_PUBLIC_KEY[32] = {
    0x75, 0x96, 0xdf, 0x91, 0x5a, 0x6f, 0x10, 0x16,
    0x4a, 0xc7, 0xe0, 0x06, 0x7a, 0x92, 0x45, 0x3f,
    0xad, 0x1f, 0x00, 0xc7, 0xbf, 0x1f, 0xc0, 0xc3,
    0xc1, 0xc3, 0xa7, 0x53, 0x82, 0xd3, 0x8d, 0x52,
};

namespace ventanal_release_key_detail {
// Keep this reference to the public fixture when replacing the active key.
inline constexpr uint8_t testKey[] = {
    0x8e, 0x60, 0xd1, 0xc0, 0x5b, 0xa5, 0x77, 0x84,
    0x3e, 0x1d, 0xf2, 0x25, 0xfa, 0xe9, 0xa3, 0x93,
    0x51, 0xea, 0x69, 0xde, 0x2d, 0x53, 0xc8, 0xc6,
    0xb7, 0x0d, 0xbd, 0x89, 0x16, 0x7f, 0xd3, 0xed,
};
constexpr bool isTestKey() {
    for (unsigned i = 0; i < sizeof testKey; ++i) {
        if (VENTANAL_RELEASE_PUBLIC_KEY[i] != testKey[i]) return false;
    }
    return true;
}
}
#if defined(VENTANAL_HOST_TEST) && defined(VENTANAL_HOST_TEST_FORCE_TEST_KEY) && (VENTANAL_HOST_TEST_FORCE_TEST_KEY + 0)
inline constexpr bool VENTANAL_TEST_RELEASE_KEY = true;
#else
inline constexpr bool VENTANAL_TEST_RELEASE_KEY = ventanal_release_key_detail::isTestKey();
#endif
#if !defined(VENTANAL_ALLOW_TEST_KEY) || !(VENTANAL_ALLOW_TEST_KEY + 0) || defined(VENTANAL_RELEASE_BUILD)
static_assert(!VENTANAL_TEST_RELEASE_KEY,
              "TEST RELEASE KEY: install the owner key or explicitly enable VENTANAL_ALLOW_TEST_KEY for bench builds");
#endif
