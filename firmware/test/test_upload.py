# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Stephen de la Vega
"""Validate custom flash offsets without opening any hardware port."""
import importlib.util
from pathlib import Path
import struct

import pytest

spec = importlib.util.spec_from_file_location("ventanal_upload", Path(__file__).parents[1] / "tools" / "upload.py")
upload = importlib.util.module_from_spec(spec)
spec.loader.exec_module(upload)


@pytest.fixture
def build(tmp_path):
    table = b"".join(
        struct.pack("<HBBII16sI", 0x50AA, *values, name.encode(), 0)
        for name, values in upload.EXPECTED.items()
    )
    (tmp_path / "ventanal.ino.partitions.bin").write_bytes(table)
    for name in ("ventanal.ino.bootloader.bin", "boot_app0.bin", "ventanal.ino.bin"):
        (tmp_path / name).write_bytes(b"test")
    return tmp_path


def test_custom_offsets(build):
    command = upload.flash_command(build, "/dev/test")
    offsets = [command[i - 1] for i, arg in enumerate(command) if arg.endswith(".bin")]
    assert offsets == ["0x0", "0x8000", "0xf000", "0x20000"]
    assert "erase-flash" not in command


def test_reject_wrong_table(build):
    table = build / "ventanal.ino.partitions.bin"
    data = bytearray(table.read_bytes())
    struct.pack_into("<I", data, 2 * 32 + 4, 0x10000)
    table.write_bytes(data)
    with pytest.raises(ValueError, match="partition table"):
        upload.flash_command(build, "/dev/test")


def test_reject_oversize_app(build):
    (build / "ventanal.ino.bin").write_bytes(b"x" * (0x400000 + 1))
    with pytest.raises(ValueError, match="image size"):
        upload.flash_command(build, "/dev/test")
