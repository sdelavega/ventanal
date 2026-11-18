# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Stephen de la Vega
"""Validate custom flash offsets without opening any hardware port."""
import csv
import importlib.util
from pathlib import Path
import re
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
    (tmp_path / "partitions.bin").write_bytes(table)
    for name in ("bootloader.bin", "boot_app0.bin", "firmware.bin"):
        (tmp_path / name).write_bytes(b"test")
    return tmp_path


def test_custom_offsets(build):
    command = upload.flash_command(build, "/dev/test")
    offsets = [command[i - 1] for i, arg in enumerate(command) if arg.endswith(".bin")]
    assert offsets == ["0x0", "0x8000", "0xf000", "0x20000"]
    assert "erase-flash" not in command


def test_reject_wrong_table(build):
    table = build / "partitions.bin"
    data = bytearray(table.read_bytes())
    struct.pack_into("<I", data, 2 * 32 + 4, 0x10000)
    table.write_bytes(data)
    with pytest.raises(ValueError, match="partition table"):
        upload.flash_command(build, "/dev/test")


def test_reject_oversize_app(build):
    (build / "firmware.bin").write_bytes(b"x" * (0x400000 + 1))
    with pytest.raises(ValueError, match="image size"):
        upload.flash_command(build, "/dev/test")


def test_partition_layout_matches_firmware_and_upload():
    sketch = Path(__file__).parents[1] / "ventanal"

    def size_bytes(size):
        if size.endswith("K"):
            return int(size[:-1]) * 1024
        if size.endswith("M"):
            return int(size[:-1]) * 1024 * 1024
        return int(size, 0)

    kinds = {"app": 0, "data": 1}
    subtypes = {"ota_0": 0x10, "ota_1": 0x11, "ota": 0, "nvs": 2, "spiffs": 0x82, "coredump": 3}
    lines = (sketch / "partitions.csv").read_text().splitlines()
    layout = {
        name: (kinds[kind], subtypes[subtype], int(offset, 0), size_bytes(size))
        for name, kind, subtype, offset, size, _ in csv.reader(
            (line for line in lines if line and not line.startswith("#")), skipinitialspace=True
        )
    }
    assert layout == upload.EXPECTED
    constants = {
        name: int(value, 0)
        for name, value in re.findall(
            r"constexpr size_t (ota\w+) = (0x[0-9a-fA-F]+);", (sketch / "ota.h").read_text()
        )
    }
    assert layout["ota_0"][3] == layout["ota_1"][3]
    assert constants == {
        "otaCacheOffset": layout["littlefs"][2],
        "otaCacheSize": layout["littlefs"][3],
        "otaSlotSize": layout["ota_0"][3],
    }
