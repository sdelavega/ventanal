#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Stephen de la Vega
#
# Proves firmware/common/hardening.h blocks permanent eFuse options unless
# both explicit opt-ins are present, and allows virtual eFuses.
set -eu
CXX="${CXX:-c++}"
here=$(cd "$(dirname "$0")" && pwd)
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
printf '#include "hardening.h"\nint main() { return VENTANAL_PERMANENT_SECURITY_REQUESTED; }\n' > "$tmp/t.cpp"

compiles() { "$CXX" -std=c++17 -fsyntax-only -I"$here/../common" "$@" "$tmp/t.cpp" 2>/dev/null; }
fail=0
expect_ok() { if compiles "$@"; then :; else echo "hardening: expected success with: $*"; fail=1; fi; }
expect_err() { if compiles "$@"; then echo "hardening: expected #error with: $*"; fail=1; fi; }

expect_ok
for option in CONFIG_SECURE_BOOT CONFIG_SECURE_BOOT_V2_ENABLED CONFIG_SECURE_FLASH_ENC_ENABLED \
              CONFIG_NVS_ENCRYPTION CONFIG_SECURE_ENABLE_SECURE_ROM_DL_MODE \
              CONFIG_SECURE_DISABLE_ROM_DL_MODE CONFIG_SECURE_BOOT_ENABLE_AGGRESSIVE_KEY_REVOKE; do
    expect_err -D"$option"=1
    expect_err -D"$option"=1 -DVENTANAL_I_ACCEPT_PERMANENT_EFUSE_CHANGES=1
    expect_err -D"$option"=1 -DVENTANAL_SECURE_BOOT_KEY_READY=1
    expect_err -D"$option"=1 -DVENTANAL_I_ACCEPT_PERMANENT_EFUSE_CHANGES=0 -DVENTANAL_SECURE_BOOT_KEY_READY=1
    expect_ok -D"$option"=1 -DVENTANAL_I_ACCEPT_PERMANENT_EFUSE_CHANGES=1 -DVENTANAL_SECURE_BOOT_KEY_READY=1
    expect_ok -D"$option"=1 -DCONFIG_EFUSE_VIRTUAL=1
done
if [ "$fail" -ne 0 ]; then exit 1; fi
echo "hardening guard: permanent eFuse options blocked without both opt-ins; virtual eFuses allowed"
