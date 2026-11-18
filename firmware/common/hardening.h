// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
#pragma once

// Guard against accidentally enabling ESP32 security features that burn
// eFuses. Secure Boot, flash encryption, NVS encryption and the ROM
// download-mode restrictions are PERMANENT once a board boots with them:
// the board can then only run images signed with your key, may never be
// reflashed over USB again, and losing the key bricks it for updates.
//
// Ventanal never enables any of this. People install it on their own hobby
// boards, and the project must never lock anyone out of their own device.
// See docs/security-hardening.md before even thinking about it.
//
// If one of these options reaches a build (for example through a copied
// sdkconfig or custom_sdkconfig line), compilation stops here unless the
// builder has made BOTH deliberate choices below:
//   1. -DVENTANAL_I_ACCEPT_PERMANENT_EFUSE_CHANGES=1
//   2. -DVENTANAL_SECURE_BOOT_KEY_READY=1 after replacing the placeholder
//      key path (see the commented [env:hardened] blocks in platformio.ini)
// Virtual eFuses (CONFIG_EFUSE_VIRTUAL) do not burn anything and are the
// recommended way to experiment; they are allowed without the opt-in.

#if defined(__has_include)
#if __has_include(<sdkconfig.h>)
#include <sdkconfig.h>
#endif
#endif

#if defined(CONFIG_SECURE_BOOT) || defined(CONFIG_SECURE_BOOT_V2_ENABLED) || \
    defined(CONFIG_SECURE_FLASH_ENC_ENABLED) || defined(CONFIG_NVS_ENCRYPTION) || \
    defined(CONFIG_SECURE_ENABLE_SECURE_ROM_DL_MODE) || \
    defined(CONFIG_SECURE_DISABLE_ROM_DL_MODE) || \
    defined(CONFIG_SECURE_BOOT_ENABLE_AGGRESSIVE_KEY_REVOKE)
#define VENTANAL_PERMANENT_SECURITY_REQUESTED 1
#else
#define VENTANAL_PERMANENT_SECURITY_REQUESTED 0
#endif

#if defined(CONFIG_EFUSE_VIRTUAL)
#define VENTANAL_VIRTUAL_EFUSES 1
#else
#define VENTANAL_VIRTUAL_EFUSES 0
#endif

#if VENTANAL_PERMANENT_SECURITY_REQUESTED && !VENTANAL_VIRTUAL_EFUSES
#if !defined(VENTANAL_I_ACCEPT_PERMANENT_EFUSE_CHANGES) || !(VENTANAL_I_ACCEPT_PERMANENT_EFUSE_CHANGES + 0)
#error "A Secure Boot / flash-encryption / NVS-encryption / ROM-download option is enabled. These burn eFuses PERMANENTLY. Ventanal never enables them. Read docs/security-hardening.md; to proceed anyway define VENTANAL_I_ACCEPT_PERMANENT_EFUSE_CHANGES=1."
#endif
#if !defined(VENTANAL_SECURE_BOOT_KEY_READY) || !(VENTANAL_SECURE_BOOT_KEY_READY + 0)
#error "Permanent security options are enabled but the signing key is still the placeholder. Replace PLACEHOLDER-secure-boot-signing-key.pem with your own key (kept offline, backed up) and define VENTANAL_SECURE_BOOT_KEY_READY=1."
#endif
#endif
