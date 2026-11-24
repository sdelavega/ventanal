#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Stephen de la Vega
"""Build unsigned Ventanal OTA assets from the tracked firmware sources."""

import argparse
from configparser import ConfigParser
import hashlib
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import sys
from tempfile import TemporaryDirectory

from ventanal.release import canonical_release_json, version_tuple


ROOT = Path(__file__).resolve().parents[2]
INKPLATE = Path("firmware/ventanal")
LCD = Path("firmware/lcd43b")
PIO_PLATFORM = "https://github.com/pioarduino/platform-espressif32/releases/download/55.03.312-1/platform-espressif32.zip"
SLOT_SIZE = 4 * 1024 * 1024


PERMANENT_SECURITY_OPTIONS = (
    "CONFIG_SECURE_BOOT", "CONFIG_SECURE_FLASH_ENC", "CONFIG_NVS_ENCRYPTION",
    "CONFIG_SECURE_ENABLE_SECURE_ROM_DL_MODE", "CONFIG_SECURE_DISABLE_ROM_DL_MODE",
    "VENTANAL_I_ACCEPT_PERMANENT_EFUSE_CHANGES",
)


def ensure_no_hardening(text, project):
    """Public releases must never carry eFuse-burning options (see docs/security-hardening.md)."""
    for number, line in enumerate(text.splitlines(), 1):
        active = line.split(";", 1)[0].split("#", 1)[0]
        if any(option in active for option in PERMANENT_SECURITY_OPTIONS):
            raise ValueError(
                f"{project}/platformio.ini:{number} enables a permanent security option; "
                "public releases never enable Secure Boot, flash or NVS encryption")


def stage_sources(work, projects):
    paths = ["firmware/common", *map(str, projects)]
    tracked = subprocess.check_output(["git", "ls-files", "-z", "--", *paths], cwd=ROOT)
    for name in tracked.decode().split("\0"):
        if not name:
            continue
        source = ROOT / name
        if source.name in {"config.h", "private_config.h", "ventanal.toml"}:
            continue
        if source.match("platformio.local*.ini"):
            continue
        if source.resolve() != source or not source.is_file():
            raise ValueError(f"Expected a regular tracked source file at {name}")
        destination = work / name
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source, destination)
    for project in projects:
        ensure_no_hardening((work / project / "platformio.ini").read_text(encoding="utf-8"), project)
        config = ConfigParser(interpolation=None)
        config.read(work / project / "platformio.ini")
        if config.get("env", "platform", fallback=None) != PIO_PLATFORM:
            raise ValueError(f"Expected pioarduino 55.03.312-1 in {project}/platformio.ini")


def prefix_maps(*directories):
    paths = {path for directory in directories for path in (directory, directory.resolve())}
    return " ".join(f'"-ffile-prefix-map={path}=."'
                    for path in sorted(paths, key=lambda path: (len(str(path)), str(path))))


def set_project_version(work, project, version):
    ini = work / project / "platformio.ini"
    text = ini.read_text(encoding="utf-8")
    if project == LCD:
        pattern = r"(?ms)(^\[lcd43b\]\s*\n(?:(?!\[).)*?^version\s*=\s*)[^\n]*"
        text, count = re.subn(pattern, lambda match: match.group(1) + version, text)
    else:
        pattern = r"(?m)^custom_sdkconfig\s*=\s*$"
        replacement = ('custom_sdkconfig =\n    CONFIG_APP_PROJECT_VER_FROM_CONFIG=y\n'
                       f'    CONFIG_APP_PROJECT_VER="{version}"')
        text, count = re.subn(pattern, lambda _: replacement, text, count=1)
    if count != 1:
        raise ValueError(f"Cannot set release version in staged {project}/platformio.ini")
    ini.write_text(text, encoding="utf-8")


def guard_image(data, board, version, sdkconfig):
    # esp_image_header_t (24 bytes), first esp_image_segment_header_t (8),
    # then esp_app_desc_t: magic at offset 32, version at offset 48.
    if len(data) < 80 or data[0] != 0xe9 or int.from_bytes(data[32:36], "little") != 0xabcd5432:
        raise ValueError(f"{board} image has no valid ESP application descriptor")
    mode, size = {"lcd43b": (2, 4), "inkplate13spectra": (3, 5)}[board]
    if data[2] != mode:
        raise ValueError(f"{board} image flash mode {data[2]:#x}, expected {mode:#x}")
    if data[3] >> 4 != size:
        raise ValueError(f"{board} image flash size {data[3] >> 4:#x}, expected {size:#x}")
    if data[48:80].split(b"\0", 1)[0] != version.encode("ascii"):
        raise ValueError(f"{board} image app descriptor version does not match {version}")
    if board == "lcd43b" and re.search(r"^CONFIG_ESPTOOLPY_OCT_FLASH=y\s*$", sdkconfig, re.MULTILINE):
        raise ValueError("lcd43b sdkconfig enables CONFIG_ESPTOOLPY_OCT_FLASH")
    if board == "lcd43b" and re.search(r"^CONFIG_MBEDTLS_INTERNAL_MEM_ALLOC=y\s*$", sdkconfig, re.MULTILINE):
        raise ValueError("lcd43b sdkconfig enables CONFIG_MBEDTLS_INTERNAL_MEM_ALLOC")
    if board == "lcd43b" and not re.search(r"^CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC=y\s*$", sdkconfig, re.MULTILINE):
        raise ValueError("lcd43b sdkconfig does not put TLS buffers in PSRAM (CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC)")


def ensure_no_home_path(data, board):
    home = str(Path.home()).encode()
    # No platform-specific strings utility is needed on the signing machine.
    # Check the actual home as well as standard prefixes from precompiled inputs.
    home_prefixes = (b"/Users/", b"/home/", b"/root/", b"/var/root/", b"\\Users\\", home)
    if any(prefix in data for prefix in home_prefixes):
        raise ValueError(f"{board} image contains a home directory path")


def write_assets(image, output, board, version, minimum, notes_url, sdkconfig=""):
    data = image.read_bytes()
    if not 0 < len(data) <= SLOT_SIZE or data[0] != 0xE9:
        raise ValueError(f"{board} did not produce an ESP application image that fits the OTA slot")
    ensure_no_home_path(data, board)
    guard_image(data, board, version, sdkconfig)
    metadata = canonical_release_json({
        "board": board,
        "version": version,
        "size": len(data),
        "sha256": hashlib.sha256(data).hexdigest(),
        "min_from_version": minimum,
        "notes_url": notes_url,
    })
    stem = output / f"ventanal-{board}-{version}"
    Path(f"{stem}.bin").write_bytes(data)
    Path(f"{stem}.json").write_bytes(metadata)


def build(args):
    if version_tuple(args.min_from_version) >= version_tuple(args.version):
        raise ValueError("--min-from-version must be older than the release")
    output = ROOT / args.output
    if output.exists():
        raise ValueError("Choose a new output directory")
    projects = {"inkplate13spectra": INKPLATE}
    if (ROOT / LCD / "platformio.ini").is_file():
        projects["lcd43b"] = LCD
    elif getattr(args, "board", None) == "lcd43b":
        raise ValueError(f"Missing {LCD}/platformio.ini")
    else:
        print(f"Skipping lcd43b: {LCD}/platformio.ini is absent.", flush=True)
    if getattr(args, "board", None):
        projects = {args.board: projects[args.board]}

    epoch = subprocess.check_output(["git", "log", "-1", "--format=%ct"], cwd=ROOT, text=True).strip()
    env = [
        "env", f"SOURCE_DATE_EPOCH={epoch}", "TZ=UTC",
        f"UV_CONSTRAINT={ROOT / 'tools/release/platformio-constraints.txt'}",
    ]
    notes_url = f"https://github.com/sdelavega/ventanal/releases/tag/v{args.version}"
    with TemporaryDirectory(prefix="ventanal-release-") as temp:
        work = Path(temp).resolve()
        assets = work / "assets"
        defines = work / "release_build.h"
        if not args.dry_run:
            stage_sources(work, projects.values())
            for project in projects.values():
                set_project_version(work, project, args.version)
            assets.mkdir()
            defines.write_text(
                '#define VENTANAL_RELEASE_BUILD 1\n'
                f'#define VENTANAL_FIRMWARE_VERSION "{args.version}"\n'
                '#ifdef __cplusplus\n'
                '#include "firmware/common/release_key.h"\n'
                '#endif\n',
                encoding="ascii",
            )
        else:
            print(f"Would stage tracked firmware, use the public Inkplate example configuration, "
                  f"and compile version {args.version} with VENTANAL_RELEASE_BUILD enabled.")

        # Keep the core, packages, cache and build paths inside the same
        # prefix map as the staged sources. Use a separate build per project.
        for board, project in projects.items():
            pio_core = work / f"pio-core-{board}"
            pio_build = work / board
            build_flags = f"{prefix_maps(work)} -Wl,--strip-debug"
            if project == INKPLATE:
                build_flags += " -DVENTANAL_GENERIC_RELEASE=1"
            command = [
                *env, f"PLATFORMIO_CORE_DIR={pio_core}", f"PLATFORMIO_BUILD_DIR={pio_build}",
                f"PLATFORMIO_BUILD_FLAGS={build_flags}",
                f'PLATFORMIO_BUILD_SRC_FLAGS=-include "{defines}"',
                getattr(args, "pio", "pio"), "run", "--project-dir", str(work / project), "--environment", "normal",
            ]
            print(shlex.join(command), flush=True)
            if args.dry_run:
                print(f"Would write {output}/ventanal-{board}-{args.version}.{{bin,json}}")
            else:
                subprocess.run(command, cwd=work, check=True)
                sdkconfig = ""
                if board == "lcd43b":
                    config = pio_core / "packages/framework-arduinoespressif32-libs/esp32s3/sdkconfig"
                    sdkconfig = config.read_text(encoding="utf-8")
                    for line in sdkconfig.splitlines():
                        if re.match(r"^(?:# )?CONFIG_(?:ESPTOOLPY_(?:OCT_FLASH|FLASHMODE_QIO)|MBEDTLS_(?:EXTERNAL|INTERNAL)_MEM_ALLOC)(?:=| is not set)", line):
                            print(f"{board} sdkconfig: {line}", flush=True)
                write_assets(pio_build / "normal/firmware.bin", assets, board,
                             args.version, args.min_from_version, notes_url, sdkconfig)
        if not args.dry_run:
            shutil.copytree(assets, output)
            print(f"Wrote unsigned firmware assets to {output}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("version", help="Release version without the v prefix")
    parser.add_argument("--min-from-version", default="0.0.0")
    parser.add_argument("--output", type=Path, required=True, help="New directory for the unsigned assets")
    parser.add_argument("--board", choices=("inkplate13spectra", "lcd43b"),
                        help="Build only this board (default: every board present)")
    parser.add_argument("--pio", default="pio", help="Path to the PlatformIO executable")
    parser.add_argument("--dry-run", action="store_true", help="Show builds and asset names without compiling")
    args = parser.parse_args()
    try:
        build(args)
    except (ValueError, OSError, subprocess.CalledProcessError) as exc:
        parser.exit(1, f"Release build failed: {exc}\n")


if __name__ == "__main__":
    main()
