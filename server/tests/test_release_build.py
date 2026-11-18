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


def test_inkplate_release_selects_generic_config(tmp_path, capsys):
    build.build(SimpleNamespace(version="2.0.0", min_from_version="1.0.0",
                                output=tmp_path / "assets", dry_run=True))

    commands = [line for line in capsys.readouterr().out.splitlines() if " pio run " in line]
    inkplate = next(line for line in commands if str(build.INKPLATE) in line)
    assert "-DVENTANAL_GENERIC_RELEASE=1" in inkplate
    assert "VENTANAL_INSTALLATION_CONFIG" not in inkplate


@pytest.mark.parametrize("suffix", [b"/Users/builder/sketch.cpp", b"/home/runner/src", b"C:\\Users\\builder\\src"])
def test_release_assets_reject_home_paths(tmp_path, suffix):
    image = tmp_path / "app.bin"
    image.write_bytes(b"\xe9\x00" + suffix + b"\x00")
    with pytest.raises(ValueError, match="home directory"):
        build.write_assets(image, tmp_path, "inkplate13spectra", "2.0.0", "1.0.0", "")
    assert list(tmp_path.iterdir()) == [image]


def test_release_rejects_active_hardening_options():
    commented = "; CONFIG_SECURE_BOOT=y\n;     CONFIG_SECURE_FLASH_ENC_ENABLED=y\n"
    build.ensure_no_hardening(commented, "firmware/x")
    for line in ("    CONFIG_SECURE_BOOT=y", "CONFIG_NVS_ENCRYPTION=y",
                 "    -DVENTANAL_I_ACCEPT_PERMANENT_EFUSE_CHANGES=1"):
        with pytest.raises(ValueError, match="permanent security option"):
            build.ensure_no_hardening(f"[env]\n{line}\n", "firmware/x")


def test_checked_in_projects_carry_no_active_hardening():
    for project in ("firmware/ventanal", "firmware/lcd43b"):
        ini = build.ROOT / project / "platformio.ini"
        if ini.is_file():
            build.ensure_no_hardening(ini.read_text(encoding="utf-8"), project)
