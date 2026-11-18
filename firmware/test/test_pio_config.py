# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Stephen de la Vega
"""Test the PlatformIO installation-header policy without a toolchain."""

from pathlib import Path
from runpy import run_path
import sys
from unittest.mock import Mock

import pytest


SCRIPT = Path(__file__).parents[1] / "ventanal" / "pio_config.py"
INSTALLATION = "VENTANAL_INSTALLATION_CONFIG"
GENERIC = "VENTANAL_GENERIC_RELEASE"


@pytest.mark.parametrize(
    "mode,has_local,defines,expected",
    [
        ("normal", True, [], None),
        ("diag", True, [], None),
        ("normal", False, [], "config.example.h"),
        ("diag", False, [], "config.example.h"),
        ("bench", True, [], "config.example.h"),
        ("normal", True, [(INSTALLATION, "config.h")], "config.h"),
        ("diag", True, [(INSTALLATION, "other.h")], "other.h"),
        ("normal", True, [(GENERIC, 1)], "config.example.h"),
        ("normal", True, [(GENERIC, 1), (INSTALLATION, "config.h")], "config.example.h"),
    ],
)
def test_installation_config(mode, has_local, defines, expected, tmp_path, capsys):
    if has_local:
        (tmp_path / "config.h").touch()
    env = Mock()
    env.IsCleanTarget.return_value = False
    env.subst.side_effect = {"$PROJECT_DIR": str(tmp_path), "$PIOENV": mode}.__getitem__
    env.Clone.return_value.get.return_value = defines
    env.Exit.side_effect = sys.exit
    script_globals = {"Import": lambda _: None, "env": env}

    if expected is None:
        with pytest.raises(SystemExit) as error:
            run_path(str(SCRIPT), init_globals=script_globals)
        assert error.value.code == 1
        output = capsys.readouterr()
        assert output.out == ""
        assert f"{mode} found config.h" in output.err
        assert "private_flags = -DVENTANAL_INSTALLATION_CONFIG=config.h" in output.err
    else:
        run_path(str(SCRIPT), init_globals=script_globals)
        output = capsys.readouterr()
        assert output.out == f"Installation config: {expected}\n"
        assert output.err == ""
