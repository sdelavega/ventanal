# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Stephen de la Vega

"""lcd43b device profile: frame format, letterbox geometry and manifests."""

from dataclasses import replace
from datetime import datetime
from http.client import HTTPConnection
import json
from pathlib import Path
from threading import Thread
from zoneinfo import ZoneInfo

import hashlib
import httpx
import numpy as np
from PIL import Image
import pytest

from ventanal import devices
from ventanal.config import ConfigError, load_config, validate_config
from ventanal.devices import lcd43b
from ventanal.manifest import read_manifest, write_manifest
from ventanal.quantize import Canvas
from ventanal.serve import make_server

TZ = ZoneInfo('America/New_York')
NOW = datetime(2026, 11, 15, 6, 0, tzinfo=TZ)
EXAMPLE = Path(__file__).parent.parent / 'ventanal.example.toml'


def cfg(tmp_path, **changes):
    return replace(
        load_config(EXAMPLE), output_dir=tmp_path / 'out',
        cache_dir=tmp_path / 'cache', **changes)


def solid_canvas(colour, size=(1600, 1200)):
    canvas = Canvas()
    canvas.rgb.paste(Image.new('RGB', size, colour), (0, 0))
    return canvas


def test_frame_is_768016_bytes_with_vnr1_header():
    data = lcd43b.encode_frame(Image.new('RGB', (800, 480)))
    assert len(data) == 768016
    assert data[:4] == b'VNR1'
    assert np.frombuffer(data[4:8], dtype='<u2').tolist() == [800, 480]
    assert data[8:16] == bytes(8)


@pytest.mark.parametrize(('colour', 'rgb565'), [
    ((255, 0, 0), 0xF800),
    ((0, 255, 0), 0x07E0),
    ((0, 0, 255), 0x001F),
    ((255, 255, 255), 0xFFFF),
    ((0, 0, 0), 0x0000),
])
def test_rgb565_little_endian_encoding(colour, rgb565):
    data = lcd43b.encode_frame(Image.new('RGB', (800, 480), colour))
    assert data[16:18] == rgb565.to_bytes(2, 'little')
    assert data[18:20] == data[16:18]


def test_decode_frame_roundtrip_and_rejects_bad_frames():
    source = np.zeros((480, 800, 3), dtype=np.uint8)
    source[:, :400] = (255, 0, 0)
    source[:, 400:] = (0, 128, 255)
    data = lcd43b.encode_frame(Image.fromarray(source))
    decoded = lcd43b.decode_frame(data)
    assert decoded.shape == (480, 800, 3)
    # RGB565 keeps five bits of red and blue and six of green.
    assert decoded[0, 0].tolist() == [248, 0, 0]
    assert decoded[0, 400].tolist() == [0, 128, 248]
    bad = [
        b'VNR2' + data[4:],
        b'VNR1' + (800).to_bytes(2, 'little') + (481).to_bytes(2, 'little') + bytes(8) + data[16:],
        data[:-1],
        data + b'\x00',
    ]
    for corrupt in bad:
        with pytest.raises(ValueError):
            lcd43b.decode_frame(corrupt)


def test_letterbox_geometry():
    frame = lcd43b.letterbox(solid_canvas((0, 255, 0)).rgb)
    assert frame.size == (800, 480)
    pixels = frame.load()
    assert pixels[0, 0] == (0, 0, 0)
    assert pixels[79, 240] == (0, 0, 0)
    assert pixels[400, 240] == (0, 255, 0)
    assert pixels[719, 479] == (0, 255, 0)
    assert pixels[720, 240] == (0, 0, 0)
    assert pixels[799, 479] == (0, 0, 0)


def test_letterbox_draws_updated_readout_in_reserved_area():
    frame = lcd43b.letterbox(solid_canvas((0, 0, 0)).rgb, NOW, clock_24h=True)
    region = np.asarray(frame.crop((574, 12, 708, 66)))
    assert region.shape == (54, 134, 3)
    assert (region != 0).any()


@pytest.mark.parametrize('clock_24h', [True, False])
def test_art_renders_native_800x480_with_credit_and_readout(clock_24h):
    candidate = dict(source='met', object_id='1', title='Snow', artist='Artist',
                     date='1900', museum='Met')
    image = Image.new('RGB', (1600, 1200), (0, 0, 0))
    frame = lcd43b.render_art(image, candidate, NOW, clock_24h=clock_24h)
    assert frame.size == (800, 480)
    pixels = np.asarray(frame)
    assert (pixels[445:480, :600] != 0).any()   # credit band and text
    assert (pixels[0:45, 700:800] != 0).any()   # updated readout, top right


def test_updated_time_respects_12_hour_clock():
    candidate = dict(source='met', object_id='1')
    twelve = lcd43b.render_art(Image.new('RGB', (16, 12)), candidate,
                               NOW.replace(hour=18), clock_24h=False)
    twenty = lcd43b.render_art(Image.new('RGB', (16, 12)), candidate,
                               NOW.replace(hour=18), clock_24h=True)
    assert twelve.tobytes() != twenty.tobytes()


def run_device_tick(monkeypatch, cfg_value, *, screens, solar=False, spares=()):
    """Drive lcd43b.tick with controlled screen sources."""
    published = {
        'version': 1,
        'screens': screens,
        'quiet': {'start': cfg_value.quiet_start, 'end': cfg_value.quiet_end},
    }
    if spares:
        published['spares'] = list(spares)
    write_manifest(cfg_value.output_dir, published)
    art_image = Image.new('RGB', (160, 120), (10, 20, 30))

    from ventanal import jobs
    monkeypatch.setattr(jobs, 'ArtPool', lambda path: type('Pool', (), {
        'get': staticmethod(lambda source, object_id: (
            {'source': source, 'object_id': object_id} if source else None)),
    })())
    monkeypatch.setattr(jobs, 'fetch_candidate',
                        lambda candidate, client, c: art_image)
    monkeypatch.setattr(jobs, 'weather_canvas',
                        lambda *a: solid_canvas((0, 255, 0)))
    if not solar:
        monkeypatch.setattr(devices.lcd43b, '_solar_allowed',
                            lambda c, n: False)
    else:
        monkeypatch.setattr(devices.lcd43b, '_solar_allowed',
                            lambda c, n: True)
        from ventanal.solar import data as solar_data
        monkeypatch.setattr(solar_data, 'fetch_solar',
                            lambda client, cache_dir, now: object())
        from ventanal.solar import render as solar_render
        monkeypatch.setattr(solar_render, 'render_solar',
                            lambda *a: solid_canvas((0, 0, 255)))
        from ventanal.weather import data as weather_data
        monkeypatch.setattr(weather_data, 'geocode',
                            lambda *a: (40.01, -105.27, 'Boulder'))
    with httpx.Client() as client:
        devices.lcd43b.tick(cfg_value, NOW, client)


def manifest_for(cfg_value, name, *, ident='a' * 16, source='met'):
    return {
        'id': ident,
        'url': f'/frames/{name}-{ident}.vnf',
        'size': 960016,
        'readout': {'x': 0, 'y': 0, 'w': 10, 'h': 10, 'style': 'shade'},
        'rendered': NOW.isoformat(timespec='seconds'),
        'credit': {'source': source, 'object_id': '1', 'title': 'Snow',
                   'artist': 'Artist', 'date': '1900', 'museum': 'Met'},
    }


def test_device_manifest_hashes_and_schema(tmp_path, monkeypatch):
    value = cfg(tmp_path)
    run_device_tick(monkeypatch, value,
                    screens={'art': manifest_for(value, 'art'),
                             'weather': manifest_for(value, 'weather')})
    root = value.output_dir / 'devices' / 'lcd43b'
    manifest = json.loads((root / 'manifest.json').read_text())
    assert manifest['version'] == 1
    assert manifest['quiet'] == {'start': value.quiet_start, 'end': value.quiet_end}
    assert list(manifest['screens']) == ['art', 'weather']
    assert 'solar' not in manifest['screens']
    for name in ('art', 'weather'):
        entry = manifest['screens'][name]
        data = (root / 'frames' / f"{name}-{entry['id']}.vnr").read_bytes()
        assert entry['size'] == len(data) == 768016
        assert entry['sha256'] == hashlib.sha256(data).hexdigest()
        assert entry['url'] == f"/devices/lcd43b/frames/{name}-{entry['id']}.vnr"
        assert entry['id'] == hashlib.sha256(data).hexdigest()[:16]


def test_device_manifest_solar_gated_by_ham(tmp_path, monkeypatch):
    value = cfg(tmp_path)
    run_device_tick(monkeypatch, value, solar=True,
                    screens={'art': manifest_for(value, 'art'),
                             'weather': manifest_for(value, 'weather')})
    manifest = json.loads((value.output_dir / 'devices' / 'lcd43b'
                          / 'manifest.json').read_text())
    assert list(manifest['screens']) == ['art', 'weather', 'solar']


def test_device_manifest_lists_optional_spares(tmp_path, monkeypatch):
    value = cfg(tmp_path)
    spare = manifest_for(value, 'spare0', ident='b' * 16)
    run_device_tick(monkeypatch, value,
                    screens={'art': manifest_for(value, 'art'),
                             'weather': manifest_for(value, 'weather')},
                    spares=[spare])
    manifest = json.loads((value.output_dir / 'devices' / 'lcd43b'
                          / 'manifest.json').read_text())
    assert len(manifest['spares']) == 1
    entry = manifest['spares'][0]
    assert entry['url'].startswith('/devices/lcd43b/frames/spare0-')
    data = (value.output_dir / 'devices' / 'lcd43b' / 'frames'
            / f"spare0-{entry['id']}.vnr").read_bytes()
    assert entry['sha256'] == hashlib.sha256(data).hexdigest()


def test_failed_screen_keeps_previous_device_entry(tmp_path, monkeypatch):
    value = cfg(tmp_path)
    run_device_tick(monkeypatch, value,
                    screens={'art': manifest_for(value, 'art'),
                             'weather': manifest_for(value, 'weather')})
    root = value.output_dir / 'devices' / 'lcd43b'
    previous = json.loads((root / 'manifest.json').read_text())

    from ventanal import jobs
    monkeypatch.setattr(jobs, 'weather_canvas',
                        lambda *a: (_ for _ in ()).throw(RuntimeError('offline')))
    run_device_tick(monkeypatch, value, screens={'art': manifest_for(value, 'art')})
    assert json.loads((root / 'manifest.json').read_text()) == previous


def test_tick_gates_on_config(tmp_path, monkeypatch):
    from ventanal import jobs
    calls = []
    monkeypatch.setattr(devices.lcd43b, 'tick',
                        lambda c, n, client: calls.append(n))
    monkeypatch.setattr(jobs, '_tick_locked',
                        lambda *a, **k: {'art': False, 'weather': False, 'pool': 0})
    off = cfg(tmp_path / 'off')
    with httpx.Client() as client:
        jobs.tick(off, NOW, client)
    assert calls == []
    on = cfg(tmp_path / 'on', lcd43b=True)
    with httpx.Client() as client:
        jobs.tick(on, NOW, client)
    assert calls == [NOW.astimezone(TZ)]


def test_config_devices_section(tmp_path):
    assert load_config(EXAMPLE).lcd43b is False
    path = tmp_path / 'enabled.toml'
    path.write_text(EXAMPLE.read_text().replace('lcd43b = false',
                                                 'lcd43b = true'))
    assert load_config(path).lcd43b is True
    bad = tmp_path / 'bad.toml'
    bad.write_text(EXAMPLE.read_text().replace('lcd43b = false',
                                               'lcd43b = "yes"'))
    with pytest.raises(ConfigError, match='devices.lcd43b'):
        load_config(bad)


def test_epaper_outputs_unchanged_when_enabled(tmp_path, monkeypatch):
    """The device profile must not alter e-paper frames or the main manifest."""

    from ventanal import jobs
    from ventanal.art.sources import Candidate

    def snapshots(enabled):
        work = tmp_path / ('on' if enabled else 'off')
        value = cfg(work, lcd43b=enabled)
        value.cache_dir.mkdir(parents=True)
        pool = jobs.ArtPool(value.cache_dir / 'art.sqlite3')
        pool.add([Candidate('met', '1', 'Snow', 'Artist', '1900', 'Met',
                            'https://example.test/art', 1800, 1200,
                            kind='painting')], 'holiday-winter')
        pool.set_refreshed(NOW)
        monkeypatch.setattr(jobs, 'fetch_image',
                            lambda *a: Image.new('RGB', (16, 12)))
        monkeypatch.setattr(jobs, 'render_art',
                            lambda *a: (solid_canvas((9, 9, 9)), {
                                'title': 'Snow', 'source': 'met',
                                'object_id': '1'}))
        monkeypatch.setattr(jobs, 'quantize',
                            lambda *a: np.zeros((1200, 1600), dtype=np.uint8))
        monkeypatch.setattr(jobs, 'simulate',
                            lambda *a: Image.new('RGB', (16, 12)))
        monkeypatch.setattr(jobs, 'geocode', lambda *a: (40.01, -105.27, 'Boulder'))
        monkeypatch.setattr(jobs, 'fetch_weather', lambda *a, **k: object())
        monkeypatch.setattr(jobs, 'fetch_alerts', lambda *a: [])
        monkeypatch.setattr(jobs, 'fetch_iem', lambda *a: None)
        monkeypatch.setattr(jobs, 'basemap', lambda *a: {'states': [], 'counties': []})
        monkeypatch.setattr(jobs, 'render_weather',
                            lambda *a: solid_canvas((8, 8, 8)))
        if enabled:
            monkeypatch.setattr(devices.lcd43b, 'tick',
                                lambda *a, **k: (_ for _ in ()).throw(
                                    RuntimeError('device failed')))
        with httpx.Client() as client:
            jobs.tick(value, NOW, client)
        files = {}
        for path in sorted((value.output_dir).rglob('*')):
            if path.is_file():
                files[str(path.relative_to(value.output_dir))] = path.read_bytes()
        return files

    disabled = snapshots(False)
    enabled = snapshots(True)
    assert set(disabled) == set(enabled)
    for name, data in disabled.items():
        assert enabled[name] == data, name


def test_serve_device_routes(tmp_path):
    root = tmp_path / 'out'
    device = root / 'devices' / 'lcd43b'
    (device / 'frames').mkdir(parents=True)
    (root / 'manifest.json').write_text('{}')
    (device / 'manifest.json').write_text('{"version":1}')
    (device / 'frames' / 'art-aaaaaaaaaaaaaaaa.vnr').write_bytes(b'frame')
    srv = make_server(root, '127.0.0.1', 0)
    thread = Thread(target=srv.serve_forever, daemon=True)
    thread.start()
    try:
        conn = HTTPConnection(*srv.server_address)
        for path, status, ctype in [
            ('/devices/lcd43b/manifest.json', 200, 'application/json'),
            ('/devices/lcd43b/frames/art-aaaaaaaaaaaaaaaa.vnr', 200,
             'application/octet-stream'),
            ('/devices/lcd43b/frames/missing.vnr', 404, None),
            ('/devices/lcd43b/../../secret', 404, None),
            ('/devices/other/manifest.json', 404, None),
            ('/manifest.json', 200, 'application/json'),
        ]:
            conn.request('GET', path)
            response = conn.getresponse()
            assert response.status == status, path
            if ctype:
                assert response.getheader('Content-Type') == ctype
            response.read()
        conn.close()
    finally:
        srv.shutdown()
        srv.server_close()
        thread.join()