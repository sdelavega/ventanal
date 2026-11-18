// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
#pragma once

#include <stdint.h>

// Active key. Install together with the server release public key.
constexpr uint8_t VENTANAL_RELEASE_PUBLIC_KEY[32] = {
    0x8e, 0x60, 0xd1, 0xc0, 0x5b, 0xa5, 0x77, 0x84,
    0x3e, 0x1d, 0xf2, 0x25, 0xfa, 0xe9, 0xa3, 0x93,
    0x51, 0xea, 0x69, 0xde, 0x2d, 0x53, 0xc8, 0xc6,
    0xb7, 0x0d, 0xbd, 0x89, 0x16, 0x7f, 0xd3, 0xed,
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
inline constexpr bool VENTANAL_TEST_RELEASE_KEY = ventanal_release_key_detail::isTestKey();
#if !defined(VENTANAL_ALLOW_TEST_KEY) || !(VENTANAL_ALLOW_TEST_KEY + 0) || defined(VENTANAL_RELEASE_BUILD)
static_assert(!VENTANAL_TEST_RELEASE_KEY,
              "TEST RELEASE KEY: install the owner key or explicitly enable VENTANAL_ALLOW_TEST_KEY for bench builds");
#endif
