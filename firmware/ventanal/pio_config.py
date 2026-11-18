# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Stephen de la Vega
"""Report the installation header and catch an unselected local config."""

from pathlib import Path
import sys

Import("env")

if not env.IsCleanTarget():
    # Resolve the project's compiler flags without changing the build environment.
    flags = env.Clone()
    flags.ProcessFlags(env.get("BUILD_FLAGS"))
    flags.ProcessUnFlags(env.get("BUILD_UNFLAGS"))
    flags.ProcessFlags(env.get("SRC_BUILD_FLAGS"))
    defines = dict(
        (define, None) if isinstance(define, str) else define
        for define in flags.get("CPPDEFINES", [])
    )
    project = Path(env.subst("$PROJECT_DIR"))
    mode = env.subst("$PIOENV")
    config = defines.get("VENTANAL_INSTALLATION_CONFIG")
    if "VENTANAL_GENERIC_RELEASE" in defines:
        config = "config.example.h"
    elif not config and mode in {"normal", "diag"} and (project / "config.h").exists():
        print(
            f"Error: {mode} found config.h but no installation header is selected. "
            "In platformio.local.ini, set [inkplate] "
            "private_flags = -DVENTANAL_INSTALLATION_CONFIG=config.h.",
            file=sys.stderr,
        )
        env.Exit(1)
    print(f"Installation config: {config or 'config.example.h'}", flush=True)
