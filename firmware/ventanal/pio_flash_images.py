# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Stephen de la Vega
"""Put Arduino's OTA selection seed alongside the other wired flash images."""

from pathlib import Path
import shutil

Import("env")

arduino = env.PioPlatform().get_package_dir("framework-arduinoespressif32")
seed = Path(arduino) / "tools/partitions/boot_app0.bin"


def copy_seed(target, source, env):
    shutil.copyfile(str(source[0]), str(target[0]))


image = env.Command("$BUILD_DIR/boot_app0.bin", str(seed), copy_seed)
env.Depends("$BUILD_DIR/${PROGNAME}.bin", image)
