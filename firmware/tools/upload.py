# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Stephen de la Vega
"""Upload a validated Ventanal build at the custom partition offsets."""
import argparse
from pathlib import Path
import struct
import subprocess
import sys

# Keep in sync with partitions.csv and ota.h (checked by test_upload.py).
EXPECTED = {
    "nvs": (1, 2, 0x9000, 24 * 1024),
    "otadata": (1, 0, 0xF000, 8 * 1024),
    "ota_0": (0, 0x10, 0x20000, 4 * 1024 * 1024),
    "ota_1": (0, 0x11, 0x420000, 4 * 1024 * 1024),
    "littlefs": (1, 0x82, 0x820000, 0x17D0000),
    "coredump": (1, 3, 0x1FF0000, 64 * 1024),
}


def flash_command(build: Path, port: str) -> list[str]:
    table = (build / "ventanal.ino.partitions.bin").read_bytes()
    entries = {}
    for pos in range(0, len(table) - 31, 32):
        magic, kind, subtype, offset, size, label, _ = struct.unpack_from("<HBBII16sI", table, pos)
        if magic != 0x50AA:
            break  # MD5/footer, not a partition.
        name = label.rstrip(b"\0").decode("ascii")
        if name in entries:
            raise ValueError("duplicate partition label")
        entries[name] = (kind, subtype, offset, size)
    if entries != EXPECTED:
        raise ValueError("build partition table does not match Ventanal's 32 MB layout")
    images = [
        (0, "ventanal.ino.bootloader.bin", 0x8000),
        (0x8000, "ventanal.ino.partitions.bin", 0x1000),
        (entries["otadata"][2], "boot_app0.bin", entries["otadata"][3]),
        (entries["ota_0"][2], "ventanal.ino.bin", entries["ota_0"][3]),
    ]
    command = [sys.executable, "-m", "esptool", "--chip", "esp32s3", "--port", port,
               "--baud", "921600", "--before", "default-reset", "--after", "hard-reset",
               "write-flash", "--flash-mode", "keep", "--flash-freq", "keep", "--flash-size", "32MB"]
    for offset, filename, limit in images:
        path = build / filename
        if not 0 < path.stat().st_size <= limit:
            raise ValueError(f"invalid image size: {filename}")
        command.extend([hex(offset), str(path)])
    return command


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("build", type=Path)
    parser.add_argument("--port", default="/dev/cu.wchusbserial10")
    parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args()
    command = flash_command(args.build.resolve(), args.port)
    if args.dry_run:
        print(" ".join(command))
    else:
        subprocess.run(command, check=True)


if __name__ == "__main__":
    main()
