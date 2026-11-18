# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Stephen de la Vega
"""Fetch the original Inkplate_S3 pin header from the pinned Soldered core."""

from hashlib import sha256
from io import BytesIO
from pathlib import Path
from tempfile import NamedTemporaryFile
from urllib.request import urlopen
from zipfile import ZipFile

Import("env")

ARCHIVE_URL = (
    "https://raw.githubusercontent.com/SolderedElectronics/"
    "Inkplate-Board-Definitions-for-Arduino-IDE/17588104356a3a76ce1c9592f2b5a496690d72f8/"
    "Inkplate_core-4.0.0.zip"
)
ARCHIVE_SHA256 = "af5a9b8408dda0e048812fbc9adecc0207f5b6f21d27b676377736f23bd10e42"
HEADER = "Inkplate_S3/pins_arduino.h"
HEADER_SHA256 = "82ad0a06ce5f4f13cd3c9d5982594626f939a3742f6056d40772fec26b54abc0"

variant_dir = env.BoardConfig().get("build.variants_dir")
header = Path(env.subst("$PROJECT_DIR")) / variant_dir / HEADER
if not header.is_file() or sha256(header.read_bytes()).hexdigest() != HEADER_SHA256:
    with urlopen(ARCHIVE_URL, timeout=120) as response:
        archive = response.read()
    if sha256(archive).hexdigest() != ARCHIVE_SHA256:
        raise ValueError("Soldered Inkplate core archive checksum mismatch")
    with ZipFile(BytesIO(archive)) as source:
        pins = source.read(f"Inkplate_core-4.0.0/variants/{HEADER}")
    if sha256(pins).hexdigest() != HEADER_SHA256:
        raise ValueError("Soldered Inkplate_S3 pin header checksum mismatch")
    header.parent.mkdir(parents=True, exist_ok=True)
    with NamedTemporaryFile(dir=header.parent, delete=False) as target:
        target.write(pins)
    Path(target.name).replace(header)
