# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Stephen de la Vega

from dataclasses import asdict, replace
from pathlib import Path
import re
import tomllib

import numpy as np
from PIL import Image
import pytest

from ventanal.calibrate import chart, measure
from ventanal.quantize import simulate

KNOWN = [(23, 28, 31), (218, 221, 209), (224, 192, 49),
         (169, 45, 39), (42, 70, 136), (46, 113, 68)]
CORNERS = [(130, 110), (1720, 35), (1810, 1350), (45, 1270)]


def test_measure_recovers():
    rows, values = [], []
    for (x, y), (u, v) in zip(CORNERS, [(0, 0), (1599, 0), (1599, 1199), (0, 1199)]):
        rows.extend([[x, y, 1, 0, 0, 0, -u*x, -u*y],
                     [0, 0, 0, x, y, 1, -v*x, -v*y]])
        values.extend([u, v])
    photo = simulate(chart(), KNOWN).transform(
        (1900, 1450), Image.Transform.PERSPECTIVE, np.linalg.solve(rows, values),
        resample=Image.Resampling.BICUBIC)
    assert np.max(np.abs(np.array(measure(photo, CORNERS)) - KNOWN)) <= 3


def test_chart_geometry():
    idx = chart()
    assert idx.shape == (1200, 1600) and idx.dtype == np.uint8
    assert [int(idx[y, x]) for y in (300, 900) for x in (290, 800, 1310)] == list(range(6))
    assert np.all(idx[:40, 40:-40] == 0)
    assert np.all(idx[-40:, 40:-40] == 0)
    assert np.all(idx[40:-40, :40] == 0)
    assert np.all(idx[40:-40, -40:] == 0)
    for y in (0, 1160):
        for x in (0, 1560):
            assert np.all(idx[y:y+40, x:x+40] == 1)


@pytest.mark.parametrize('corners', [CORNERS[:3], [(0, 0)] * 4,
    [(0, 0), (1599, 1199), (1599, 0), (0, 1199)],
    [(-1, 0), (1599, 0), (1599, 1199), (0, 1199)]])
def test_invalid_corners(corners):
    with pytest.raises(ValueError, match='corners'):
        measure(Image.new('RGB', (1900, 1450)), corners)


@pytest.fixture
def cli_config(tmp_path, monkeypatch):
    from ventanal import cli
    from ventanal.config import load_config
    cfg = replace(load_config(Path('ventanal.example.toml')),
                  output_dir=tmp_path / 'out', cache_dir=tmp_path / 'cache')
    monkeypatch.setattr(cli, 'load_config', lambda path: cfg)
    monkeypatch.setattr(cli.httpx, 'Client', lambda *a, **k: pytest.fail('offline command used HTTP'))
    return cfg


def test_chart_cli_publishes(cli_config):
    from ventanal.cli import main
    from ventanal.frame import decode_frame
    from ventanal.manifest import read_manifest
    from ventanal.art.render import ART_READOUT
    main(['chart', '--config', 'example.toml'])
    entry = read_manifest(cli_config.output_dir)['screens']['art']
    assert entry['credit'] == {'title': 'Calibration chart', 'artist': 'Ventanal', 'date': '', 'museum': ''}
    assert entry['readout'] == asdict(ART_READOUT)
    np.testing.assert_array_equal(decode_frame((cli_config.output_dir / entry['url'].lstrip('/')).read_bytes()), chart())
    with Image.open(cli_config.output_dir / 'previews' / (Path(entry['url']).stem + '.png')) as preview:
        np.testing.assert_array_equal(preview, simulate(chart(), cli_config.panel_rgb))
    assert not cli_config.cache_dir.exists()


@pytest.mark.parametrize('write', [False, True])
@pytest.mark.parametrize('header', ['[panel]', '[\"panel\"]', "['panel']"])
def test_calibrate_cli_preserves_config(cli_config, tmp_path, capsys, write, header):
    from ventanal.cli import main
    photo = tmp_path / 'photo.png'
    simulate(chart(), KNOWN).save(photo)
    config = tmp_path / 'settings.toml'
    original = (b'# untouched\r\n[other]\r\ntext = """\r\n[panel]\r\nblack = \'fake\'\r\n"""\r\n'
                + header.encode() + b' # measurements\r\nextra = [\r\n[1],\r\n[2]\r\n]\r\n' + b''.join(
        f'  "{key}" = \'#000000\' # keep {key}\r\n'.encode()
        for key in ('black', 'white', 'yellow', 'red', 'blue', 'green')) +
        b'unknown = "keep"\r\n[after]\r\nvalue = 12 # unchanged\r\n')
    config.write_bytes(original)
    args = ['--config', str(config), 'calibrate', str(photo), '--corners', '0,0 1599,0 1599,1199 0,1199']
    main(args + (['--write'] if write else []))
    expected = {key: '#' + ''.join(f'{c:02x}' for c in rgb)
                for key, rgb in zip(('black', 'white', 'yellow', 'red', 'blue', 'green'), KNOWN)}
    assert tomllib.loads(capsys.readouterr().out)['panel'] == expected
    actual = config.read_bytes()
    if write:
        assert tomllib.loads(actual.decode())['panel'] == {**expected, 'unknown': 'keep', 'extra': [[1], [2]]}
        assert re.sub(rb'#[0-9a-f]{6}', b'#000000', actual) == original
    else:
        assert actual == original
    assert not cli_config.output_dir.exists()
    assert not cli_config.cache_dir.exists()


def test_measure_uses_central_median():
    pixels = np.asarray(simulate(chart(), KNOWN)).copy()
    # Less than half the central area is contaminated, defeating a mean.
    pixels[::4] = 255
    assert measure(Image.fromarray(pixels), [(0, 0), (1599, 0), (1599, 1199), (0, 1199)]) == KNOWN


def test_write_panel_loadable_example(tmp_path):
    from ventanal.calibrate import write_panel
    from ventanal.config import load_config
    config = tmp_path / 'example.toml'
    config.write_bytes(Path('ventanal.example.toml').read_bytes())
    before = load_config(config)
    write_panel(config, KNOWN)
    assert load_config(config) == replace(before, panel_rgb=tuple(KNOWN))
