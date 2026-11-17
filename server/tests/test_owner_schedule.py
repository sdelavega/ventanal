# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Stephen de la Vega
from pathlib import Path
import pytest
from ventanal.config import load_config, ConfigError

EXAMPLE = Path(__file__).parents[1] / 'ventanal.example.toml'

def test_quiet_defaults():
    cfg = load_config(EXAMPLE)
    assert (cfg.quiet_start, cfg.quiet_end) == ('23:00', '06:00')

@pytest.mark.parametrize('value', ['24:00', '6:00', '12:60', 'no'])
def test_invalid_quiet(tmp_path, value):
    path = tmp_path / 'test.toml'
    path.write_text(EXAMPLE.read_text().replace('quiet_start = "23:00"\n', '').replace('[schedule]', f'[schedule]\nquiet_start = "{value}"'))
    with pytest.raises(ConfigError, match='schedule.quiet_start'):
        load_config(path)
