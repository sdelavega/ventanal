#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Stephen de la Vega
"""Build unsigned Ventanal OTA assets from the tracked firmware sources."""

import argparse
from configparser import ConfigParser
import hashlib
import json
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
CORE = "soldered-inkplate-boards:esp32"
CORE_VERSION = "4.0.0"
LIBRARIES = {"InkplateLibrary": "11.2.1", "ArduinoJson": "7.4.3"}
FQBN = f"{CORE}:Inkplate13SPECTRA:FlashSize=32M,FlashMode=opi,PSRAM=opi,PartitionScheme=custom,DebugLevel=none"
PIO_PLATFORM = "https://github.com/pioarduino/platform-espressif32/releases/download/55.03.312-1/platform-espressif32.zip"
SLOT_SIZE = 4 * 1024 * 1024


def check_inkplate_toolchain():
    def arduino_json(*args):
        return json.loads(subprocess.check_output(["arduino-cli", *args, "--json"], cwd=ROOT))

    platforms = {p["id"]: p["installed_version"] for p in arduino_json("core", "list")["platforms"]}
    if platforms.get(CORE) != CORE_VERSION:
        raise ValueError(f"Install Arduino core {CORE}@{CORE_VERSION}")
    installed = arduino_json("lib", "list")["installed_libraries"]
    libraries = {entry["library"]["name"]: entry["library"]["version"] for entry in installed}
    for name, version in LIBRARIES.items():
        if libraries.get(name) != version:
            raise ValueError(f"Install Arduino library {name}@{version}")


def stage_sources(work, has_lcd):
    paths = ["firmware/common", str(INKPLATE)]
    if has_lcd:
        paths.append(str(LCD))
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
    sketch = work / INKPLATE
    shutil.copyfile(sketch / "config.example.h", sketch / "config.h")
    if has_lcd:
        config = ConfigParser(interpolation=None)
        config.read(work / LCD / "platformio.ini")
        if config.get("env", "platform", fallback=None) != PIO_PLATFORM:
            raise ValueError(f"Expected pioarduino 55.03.312-1 in {LCD}/platformio.ini")


def write_assets(image, output, board, version, minimum, notes_url):
    data = image.read_bytes()
    if not 0 < len(data) <= SLOT_SIZE or data[0] != 0xE9:
        raise ValueError(f"{board} did not produce an ESP application image that fits the OTA slot")
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
    if args.output.exists():
        raise ValueError("Choose a new output directory")
    lcd_exists = (ROOT / LCD / "platformio.ini").is_file()
    if args.board == "lcd43b" and not lcd_exists:
        raise ValueError(f"Missing {LCD}/platformio.ini")
    has_lcd = lcd_exists and args.board != "inkplate13spectra"
    has_inkplate = args.board != "lcd43b"
    if not lcd_exists and args.board is None:
        print(f"Skipping lcd43b: {LCD}/platformio.ini is absent.", flush=True)
    if has_inkplate and not args.dry_run:
        check_inkplate_toolchain()

    epoch = subprocess.check_output(["git", "log", "-1", "--format=%ct"], cwd=ROOT, text=True).strip()
    env = ["env", f"SOURCE_DATE_EPOCH={epoch}", "TZ=UTC"]
    notes_url = f"https://github.com/sdelavega/ventanal/releases/tag/v{args.version}"
    with TemporaryDirectory(prefix="ventanal-release-") as temp:
        work = Path(temp).resolve()
        sketch = work / INKPLATE
        build_dir = work / "inkplate-build"
        assets = work / "assets"
        defines = work / "release_build.h"
        stage_sources(work, has_lcd)
        if not args.dry_run:
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
            print(f"Would compile staged firmware at version {args.version} "
                  f"with VENTANAL_RELEASE_BUILD enabled.")

        def compile_board(command, image, board):
            print(shlex.join(command), flush=True)
            if args.dry_run:
                print(f"Would write {args.output}/ventanal-{board}-{args.version}.{{bin,json}}")
            else:
                subprocess.run(command, cwd=work, check=True)
                write_assets(image, assets, board, args.version, args.min_from_version, notes_url)

        prefix_map = f'"-ffile-prefix-map={work}=."'
        cpp_flags = f'{prefix_map} -include "{defines}" "-I{sketch}"'
        if has_inkplate:
            compile_board([
                *env, "arduino-cli", "compile", "--fqbn", FQBN, "--build-path", str(build_dir),
                "--build-property", "upload.maximum_size=4194304",
                "--build-property", "recipe.hooks.objcopy.postobjcopy.3.pattern=/usr/bin/true",
                "--build-property", "recipe.hooks.objcopy.postobjcopy.4.pattern=/usr/bin/true",
                # elf2image embeds the ELF digest. Discard debug paths before hashing.
                "--build-property", "compiler.c.elf.extra_flags=-Wl,--strip-debug",
                "--build-property", f"compiler.S.extra_flags={prefix_map}",
                "--build-property", f"compiler.c.extra_flags={prefix_map}",
                "--build-property", f"compiler.cpp.extra_flags={cpp_flags}",
                str(sketch),
            ], build_dir / "ventanal.ino.bin", "inkplate13spectra")
        if has_lcd:
            project = work / LCD
            compile_board([
                *env, f"PLATFORMIO_BUILD_FLAGS={prefix_map} -Wl,--strip-debug",
                f'PLATFORMIO_BUILD_SRC_FLAGS=-include "{defines}"',
                args.pio, "run", "--project-dir", str(project), "--environment", "normal",
            ], project / ".pio/build/normal/firmware.bin", "lcd43b")
        if not args.dry_run:
            shutil.copytree(assets, args.output)
            print(f"Wrote unsigned firmware assets to {args.output}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("version", help="Release version without the v prefix")
    parser.add_argument("--min-from-version", default="0.0.0")
    parser.add_argument("--output", type=Path, required=True, help="New directory for the unsigned assets")
    parser.add_argument("--board", choices=("inkplate13spectra", "lcd43b"),
                        help="Build one board (default: all available boards)")
    parser.add_argument("--pio", default="pio", help="Path to the PlatformIO executable")
    parser.add_argument("--dry-run", action="store_true", help="Show builds and asset names without compiling")
    args = parser.parse_args()
    try:
        build(args)
    except (ValueError, OSError, subprocess.CalledProcessError) as exc:
        parser.exit(1, f"Release build failed: {exc}\n")


if __name__ == "__main__":
    main()
