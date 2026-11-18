# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Stephen de la Vega
import importlib.util
from pathlib import Path
from types import SimpleNamespace

import pytest


ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("ventanal_release_build", ROOT / "tools/release/build.py")
build = importlib.util.module_from_spec(spec)
spec.loader.exec_module(build)


def test_output_directory_is_relative_to_checkout(tmp_path, monkeypatch):
    checkout = tmp_path / "checkout"
    elsewhere = tmp_path / "elsewhere"
    checkout.mkdir()
    elsewhere.mkdir()
    (checkout / "occupied").mkdir()
    monkeypatch.chdir(elsewhere)
    monkeypatch.setattr(build, "ROOT", checkout)
    with pytest.raises(ValueError, match="new output directory"):
        build.build(SimpleNamespace(version="2.0.0", min_from_version="1.0.0", output=Path("occupied")))


@pytest.mark.parametrize("suffix", [b"/Users/builder/sketch.cpp", b"/home/runner/src", b"C:\\Users\\builder\\src"])
def test_release_assets_reject_home_paths(tmp_path, suffix):
    image = tmp_path / "app.bin"
    image.write_bytes(b"\xe9\x00" + suffix + b"\x00")
    with pytest.raises(ValueError, match="home directory"):
        build.write_assets(image, tmp_path, "inkplate13spectra", "2.0.0", "1.0.0", "")
    assert list(tmp_path.iterdir()) == [image]
