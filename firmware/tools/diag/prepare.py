# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Stephen de la Vega
"""Copy the pinned Inkplate library into scratch space and apply the logging patch."""
import argparse
import hashlib
from pathlib import Path
import shutil
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("scratch", type=Path)
parser.add_argument(
    "--library", type=Path,
    default=Path(__file__).resolve().parents[2] / "ventanal/.pio/libdeps/normal/InkplateLibrary",
)
args = parser.parse_args()
source = args.library.resolve()
target = args.scratch.resolve() / "InkplateLibrary"
driver = source / "src/boards/Inkplate13SPECTRA/Inkplate13SPECTRADriver.cpp"
expected = "28fe49afb78b3771a58e3e6e5f1d50621eb1022c33bcf760e1befdd137360507"
if hashlib.sha256(driver.read_bytes()).hexdigest() != expected:
    raise SystemExit("Driver differs from the validated 11.2.1 source; refusing patch.")
shutil.copytree(source, target)  # Refuse an existing destination.
subprocess.run(["patch", "-p1", "-i", str(Path(__file__).with_name("busy.patch").resolve())],
               cwd=target, check=True)
print(f"Scratch library prepared at {target}")
