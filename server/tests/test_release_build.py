# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Stephen de la Vega
import importlib.util
import shlex
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


def image(mode, size, version):
    data = bytearray(24 + 8 + 256)
    data[0] = 0xe9
    data[2] = mode
    data[3] = size << 4
    data[24:28] = (0x3c000020).to_bytes(4, "little")
    data[28:32] = (256).to_bytes(4, "little")
    data[32:36] = (0xabcd5432).to_bytes(4, "little")
    encoded = version.encode("ascii")
    data[48:48 + len(encoded)] = encoded
    return bytes(data)


def test_cores_are_per_board(tmp_path, capsys):
    build.build(SimpleNamespace(version="2.0.0", min_from_version="1.0.0",
                                output=tmp_path / "assets", dry_run=True))
    commands = [shlex.split(line) for line in capsys.readouterr().out.splitlines() if " pio run " in line]
    cores = [next(part for part in command if part.startswith("PLATFORMIO_CORE_DIR=")) for command in commands]
    assert len(cores) == 2 and len(set(cores)) == 2


def test_lcd_version_label_set_in_staged_ini(tmp_path):
    ini = tmp_path / "firmware/lcd43b/platformio.ini"
    ini.parent.mkdir(parents=True)
    ini.write_text('[lcd43b]\nversion = 1.2.0\nother = yes\n')
    build.set_project_version(tmp_path, build.LCD, "2.0.0")
    assert 'version = 2.0.0' in ini.read_text()
    assert 'other = yes' in ini.read_text()


def test_inkplate_staged_ini_sets_app_descriptor_version(tmp_path):
    ini = tmp_path / "firmware/ventanal/platformio.ini"
    ini.parent.mkdir(parents=True)
    ini.write_text('[env]\ncustom_sdkconfig =\n    CONFIG_OTHER=y\n\n[env:normal]\n')
    build.set_project_version(tmp_path, build.INKPLATE, "2.0.0")
    assert 'CONFIG_APP_PROJECT_VER_FROM_CONFIG=y' in ini.read_text()
    assert 'CONFIG_APP_PROJECT_VER="2.0.0"' in ini.read_text()


LCD_SDKCONFIG = "CONFIG_ESPTOOLPY_FLASHMODE_QIO=y\nCONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC=y\n"


def test_guard_rejects_lcd_internal_and_external_tls_allocators():
    with pytest.raises(ValueError, match="INTERNAL_MEM_ALLOC"):
        build.guard_image(image(2, 4, "2.0.0"), "lcd43b", "2.0.0",
                          LCD_SDKCONFIG + "CONFIG_MBEDTLS_INTERNAL_MEM_ALLOC=y\n")


def test_guard_rejects_lcd_tls_buffers_in_internal_ram():
    with pytest.raises(ValueError, match="INTERNAL_MEM_ALLOC"):
        build.guard_image(image(2, 4, "2.0.0"), "lcd43b", "2.0.0",
                          "CONFIG_ESPTOOLPY_FLASHMODE_QIO=y\nCONFIG_MBEDTLS_INTERNAL_MEM_ALLOC=y\n")


def test_guard_rejects_wrong_flash_mode():
    with pytest.raises(ValueError, match="flash mode"):
        build.guard_image(image(1, 4, "2.0.0"), "lcd43b", "2.0.0", LCD_SDKCONFIG)


def test_guard_rejects_app_version_mismatch():
    with pytest.raises(ValueError, match="version"):
        build.guard_image(image(2, 4, "1.2.0"), "lcd43b", "2.0.0", LCD_SDKCONFIG)


def test_guard_accepts_matching_images():
    build.guard_image(image(2, 4, "2.0.0"), "lcd43b", "2.0.0", LCD_SDKCONFIG)
    build.guard_image(image(3, 5, "2.0.0"), "inkplate13spectra", "2.0.0", "")
    with pytest.raises(ValueError, match="OCT_FLASH"):
        build.guard_image(image(2, 4, "2.0.0"), "lcd43b", "2.0.0", LCD_SDKCONFIG + "CONFIG_ESPTOOLPY_OCT_FLASH=y\n")
