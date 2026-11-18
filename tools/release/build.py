#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Stephen de la Vega
"""Build unsigned Ventanal OTA assets from the tracked firmware sources."""

import argparse
from configparser import ConfigParser
import hashlib
from pathlib import Path
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
        config = ConfigParser(interpolation=None)
        config.read(work / project / "platformio.ini")
        if config.get("env", "platform", fallback=None) != PIO_PLATFORM:
            raise ValueError(f"Expected pioarduino 55.03.312-1 in {project}/platformio.ini")


def prefix_maps(*directories):
    paths = {path for directory in directories for path in (directory, directory.resolve())}
    return " ".join(f'"-ffile-prefix-map={path}=."'
                    for path in sorted(paths, key=lambda path: (len(str(path)), str(path))))


def ensure_no_home_path(data, board):
    home = str(Path.home()).encode()
    # No platform-specific strings utility is needed on the signing machine.
    # Check the actual home as well as standard prefixes from precompiled inputs.
    home_prefixes = (b"/Users/", b"/home/", b"/root/", b"/var/root/", b"\\Users\\", home)
    if any(prefix in data for prefix in home_prefixes):
        raise ValueError(f"{board} image contains a home directory path")


def write_assets(image, output, board, version, minimum, notes_url):
    data = image.read_bytes()
    if not 0 < len(data) <= SLOT_SIZE or data[0] != 0xE9:
        raise ValueError(f"{board} did not produce an ESP application image that fits the OTA slot")
    ensure_no_home_path(data, board)
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
    else:
        print(f"Skipping lcd43b: {LCD}/platformio.ini is absent.", flush=True)

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
        pio_core = work / "pio-core"
        for board, project in projects.items():
            pio_build = work / board
            command = [
                *env, f"PLATFORMIO_CORE_DIR={pio_core}", f"PLATFORMIO_BUILD_DIR={pio_build}",
                f"PLATFORMIO_BUILD_FLAGS={prefix_maps(work)} -Wl,--strip-debug",
                f'PLATFORMIO_BUILD_SRC_FLAGS=-include "{defines}"',
                "pio", "run", "--project-dir", str(work / project), "--environment", "normal",
            ]
            print(shlex.join(command), flush=True)
            if args.dry_run:
                print(f"Would write {output}/ventanal-{board}-{args.version}.{{bin,json}}")
            else:
                subprocess.run(command, cwd=work, check=True)
                write_assets(pio_build / "normal/firmware.bin", assets, board,
                             args.version, args.min_from_version, notes_url)
        if not args.dry_run:
            shutil.copytree(assets, output)
            print(f"Wrote unsigned firmware assets to {output}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("version", help="Release version without the v prefix")
    parser.add_argument("--min-from-version", default="0.0.0")
    parser.add_argument("--output", type=Path, required=True, help="New directory for the unsigned assets")
    parser.add_argument("--dry-run", action="store_true", help="Show builds and asset names without compiling")
    args = parser.parse_args()
    try:
        build(args)
    except (ValueError, OSError, subprocess.CalledProcessError) as exc:
        parser.exit(1, f"Release build failed: {exc}\n")


if __name__ == "__main__":
    main()
