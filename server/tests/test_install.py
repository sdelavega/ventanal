# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Stephen de la Vega
import shlex
import subprocess
import sys
from pathlib import Path

import pytest


@pytest.mark.parametrize('live, expected', [
    ('gap', 'art.seasons: no season covers 02-29'),
    ('unparseable', 'config.file: unable to read valid TOML'),
    ('valid', None),
    ('missing', None),
])
def test_install_preflights_live_config_before_installation(tmp_path, live, expected):
    source = Path(__file__).parents[1]
    deploy = tmp_path / 'deploy'
    deploy.mkdir()
    (tmp_path / 'src').symlink_to(source / 'src')
    config = tmp_path / 'ventanal.toml'
    valid = (source / 'ventanal.example.toml').read_text()
    backups = tmp_path / 'backups'
    backups.mkdir()
    (backups / 'ventanal-20000101T000000.000000Z.toml').write_text(valid)
    if live == 'gap':
        config.write_text(valid.replace('end = "02-29"', 'end = "02-28"'))
    elif live == 'unparseable':
        config.write_text('[broken\n')
    elif live == 'valid':
        config.write_text(valid)

    # Stop before any installer side effect, even if the preflight is bypassed.
    guard = f'''
id() {{ echo 0; }}
python3() {{
    if [ "$1" = -I ]; then
        {shlex.quote(sys.executable)} "$@"
    else
        exit 87
    fi
}}
'''
    script = (source / 'deploy/install.sh').read_text()
    script = script.replace('set -eu\n', 'set -eu\n' + guard, 1)
    script = script.replace('CONFIG=/etc/ventanal/ventanal.toml', f'CONFIG={shlex.quote(str(config))}', 1)
    install = deploy / 'install.sh'
    install.write_text(script)
    result = subprocess.run(['/bin/sh', str(install)], capture_output=True, text=True, timeout=10)
    if expected is None:
        assert result.returncode == 87, result.stderr
    else:
        assert result.returncode == 1
        assert 'Install stopped before pip install' in result.stderr
        assert expected in result.stderr
