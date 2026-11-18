# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Stephen de la Vega

"""LCD frame format, native weather layout, artwork and device manifests."""

from dataclasses import replace
from datetime import datetime
from http.client import HTTPConnection
import json
from pathlib import Path
from threading import Thread
from zoneinfo import ZoneInfo

import hashlib
import zlib
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
from ventanal.art.compose import compose
from test_weather_render import sample  # shared deterministic weather fixture

TZ = ZoneInfo('America/New_York')
LAT, LON = 29.9144193, -81.3628995
PLACE = 'St. Augustine'
NOW = datetime(2026, 11, 15, 6, 0, tzinfo=TZ)
EXAMPLE = Path(__file__).parent.parent / 'ventanal.example.toml'


def cfg(tmp_path, **changes):
    changes.setdefault('zip', '32084')
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


def solar_sample(tmp_path, **changes):
    from test_solar import sample, NOW as solar_now
    from ventanal.devices.lcd_solar import render_lcd_solar
    config = cfg(tmp_path, ham_call_sign='CALL SIGN', latitude=LAT,
                 longitude=LON, **changes)
    return render_lcd_solar(sample(), config, solar_now)


def test_lcd_solar_is_native_full_screen(tmp_path):
    from PIL import ImageColor
    from ventanal.devices.lcd_lcars import CLOCK_GAP, LCD_COLOURS
    image = solar_sample(tmp_path)
    assert image.size == (800, 480) and image.mode == 'RGB'
    assert image.getpixel((10, 240)) == ImageColor.getrgb(LCD_COLOURS['gold'])
    x, y, w, h = CLOCK_GAP
    assert not np.asarray(image.crop((x, y, x+w, y+h))).any()
    assert image.getpixel((770, 13)) == ImageColor.getrgb('#ff7700')


def test_lcd_solar_golden(tmp_path):
    image = solar_sample(tmp_path)
    with Image.open(Path(__file__).parent / 'golden/lcd43b/solar.png') as expected:
        assert image.tobytes() == expected.convert('RGB').tobytes()


def test_lcd_solar_still_gated(tmp_path, monkeypatch):
    from ventanal.solar.licence import Verification
    from ventanal.solar import data as solar_data
    from test_solar import sample as solar_data_sample
    monkeypatch.setattr('ventanal.solar.licence.verify',
                        lambda *a: Verification(True, 'test licence'))
    value = cfg(tmp_path, ham_call_sign='CALL SIGN', latitude=LAT, longitude=LON)
    monkeypatch.setattr(lcd43b, 'weather_frame',
                        lambda *a: Image.new('RGB', (800, 480)))
    monkeypatch.setattr(solar_data, 'fetch_solar', lambda *a: solar_data_sample())
    lcd43b.tick(value, NOW, None)
    assert 'solar' in read_manifest(lcd43b.device_dir(value))['screens']
    # Explicitly revoke the licence for the second publication phase.
    monkeypatch.setattr('ventanal.solar.licence.verify',
                        lambda *a: Verification(False, 'test licence'))
    monkeypatch.setattr(solar_data, 'fetch_solar',
                        lambda *a: pytest.fail('unauthorised solar fetch'))
    lcd43b.tick(value, NOW, None)
    assert 'solar' not in read_manifest(lcd43b.device_dir(value))['screens']
    assert not lcd43b._solar_allowed(replace(value, ham_call_sign=''), NOW)


@pytest.mark.parametrize('located', [True, False])
def test_lcd_solar_data_pipeline(tmp_path, monkeypatch, located):
    from ventanal.solar import data as solar_data
    from ventanal.weather import data as weather_data
    from ventanal.devices.lcd_solar import render_lcd_solar
    from test_solar import sample as solar_data_sample
    data = solar_data_sample()
    config = cfg(tmp_path, ham_call_sign='CALL SIGN',
                 latitude=LAT if located else None,
                 longitude=LON if located else None)
    monkeypatch.setattr(solar_data, 'fetch_solar', lambda *a: data)

    def geocode(*args):
        assert not located
        return LAT, LON, PLACE

    monkeypatch.setattr(weather_data, 'geocode', geocode)
    image = lcd43b._render_screen(config, 'solar', NOW, None)
    expected = render_lcd_solar(data, replace(config, latitude=LAT, longitude=LON), NOW)
    assert image.tobytes() == expected.tobytes()


@pytest.mark.parametrize('storm', [False, True])
def test_lcd_solar_text_is_bounded(tmp_path, monkeypatch, storm):
    from PIL import ImageDraw
    from ventanal.devices.lcd_solar import render_lcd_solar
    from ventanal.devices.lcd_draw import LCDDraw
    from test_solar import sample as solar_data_sample
    data = solar_data_sample()
    if storm:
        data = replace(data, sfi=999, ssn=999, xray='X99.9', k=9, a=999,
                       kp=[(stamp, 9) for stamp, _ in data.kp],
                       alerts=['WARNING: G5 GEOMAGNETIC STORM · UNTIL NOV 19 2026 UTC ' * 10,
                               'WARNING: R5 RADIO BLACKOUT · NOV 19 2026 UTC'])
    else:
        data = replace(data, alerts=[], scales={})
    boxes = []
    shapes = []
    pastes = []
    original = ImageDraw.ImageDraw.text

    def capture(draw, xy, text, *args, **kwargs):
        box = draw.textbbox(xy, text, font=kwargs['font'], anchor=kwargs['anchor'])
        boxes.append((text, box))
        return original(draw, xy, text, *args, **kwargs)

    def capture_shape(method):
        # Measure destination coordinates, not the internal 4x coverage mask.
        surface = LCDDraw if method == 'rounded_rectangle' else ImageDraw.ImageDraw
        original_shape = getattr(surface, method)

        def capture(draw, xy, *args, **kwargs):
            shapes.append((method, xy))
            return original_shape(draw, xy, *args, **kwargs)

        return capture

    original_paste = Image.Image.paste

    def capture_paste(image, source, box, *args, **kwargs):
        if isinstance(source, Image.Image):
            pastes.append((*box, box[0]+source.width, box[1]+source.height))
        return original_paste(image, source, box, *args, **kwargs)

    monkeypatch.setattr(ImageDraw.ImageDraw, 'text', capture)
    for method in ('rounded_rectangle', 'line'):
        surface = LCDDraw if method == 'rounded_rectangle' else ImageDraw.ImageDraw
        monkeypatch.setattr(surface, method, capture_shape(method))
    monkeypatch.setattr(Image.Image, 'paste', capture_paste)
    config = cfg(tmp_path, ham_call_sign='CALL SIGN', latitude=LAT, longitude=LON)
    image = render_lcd_solar(data, config, NOW)
    for text, (x0, y0, x1, y1) in boxes:
        assert 0 <= x0 < x1 <= 800 and 26 <= y0 < y1 < 458, text
    for i, (text, a) in enumerate(boxes):
        for other, b in boxes[i+1:]:
            assert a[2] <= b[0] or b[2] <= a[0] or a[3] <= b[1] or b[3] <= a[1], (text, other)
    sun, = pastes
    pills = [xy for kind, xy in shapes
             if kind == 'rounded_rectangle' and xy[0] >= 270 and 218 <= xy[1] < 310]
    bars = [xy for kind, xy in shapes
            if kind == 'rounded_rectangle' and xy[1] >= 326]
    lines = [xy for kind, xy in shapes if kind == 'line']
    assert len(pills) == 8 and len(bars) == 8
    assert sun == (582, 84, 768, 270)
    assert max(box[2] for box in pills) < sun[0]
    for x0, y0, x1, y1 in pills:
        assert 140 <= x0 < x1 <= 786 and 26 <= y0 < y1 < 458
    for x0, y0, x1, y1 in bars + lines:
        assert 140 <= x0 < x1 <= 786 and 330 <= y0 <= y1 <= 353

    axis = sorted((box for _, box in boxes
                   if box[2] == 164 and box[1] >= 310), key=lambda box: box[1])
    assert len(axis) >= 2
    for upper, lower in zip(axis, axis[1:]):
        assert lower[1] - upper[3] >= 2
    row_tops = sorted({box[1] for box in pills})
    assert len(row_tops) == 4
    assert all(lower - upper >= 21 for upper, lower in zip(row_tops, row_tops[1:]))
    hf_caption, = [box for text, box in boxes if text.startswith('HF CONDITIONS')]
    # Pillow rectangles include their bottom pixel; text bboxes exclude it.
    assert hf_caption[1] - (max(box[3] for box in pills) + 1) >= 6
    assert min(line[1] for line in lines) - hf_caption[3] >= 6

    caption = [box for text, box in boxes
               if text.startswith(('PLANETARY KP', 'G1 (KP 5)'))]
    forecast = [box for text, box in boxes
                if text.startswith(('KP MAX', 'R1')) or box[0] >= 140 and 375 <= box[1] < 400]
    alert = [box for _, box in boxes if box[1] >= 430]
    assert len(caption) == 2 and len(forecast) == 9 and alert
    # Measure rendered cap heights so the forecast cannot shrink to fit again.
    headers = [box for text, box in boxes if box in forecast
               and not text.startswith(('KP MAX', 'R1'))]
    kp_lines = [box for text, box in boxes if text.startswith('KP MAX')]
    probability_lines = [box for text, box in boxes if text.startswith('R1')]
    assert len(headers) == len(kp_lines) == len(probability_lines) == 3
    for group, minimum in ((headers, 12), (kp_lines, 13), (probability_lines, 12)):
        assert all(box[3] - box[1] >= minimum for box in group)
    hours = [box for text, box in boxes if text.isdigit() and box[3] == 365]
    assert len(hours) == 8
    assert min(box[1] for box in caption) - max(box[3] for box in hours) == 1
    assert caption[0][1] == caption[1][1]
    assert min(box[1] for box in forecast) - max(box[3] for box in caption) >= 3
    assert min(box[1] for box in alert) - max(box[3] for box in forecast) >= 8
    assert 458 - max(box[3] for box in alert) >= 8
    from ventanal.devices.lcd_solar import SECTIONS
    assert 430 - (SECTIONS[-1][0] + SECTIONS[-1][1]) == 6
    assert image.size == (800, 480)


def test_lcd_frames_have_no_updated_readout():
    image = Image.new('RGB', (1600, 1200), (80, 110, 140))
    frame = lcd43b.render_art(image, {'title': 'Snow'})
    assert frame.size == (800, 480)
    assert frame.crop((550, 0, 800, 80)).tobytes() == compose(
        image, size=(800, 480)).crop((550, 0, 800, 80)).tobytes()
    assert frame.crop((0, 445, 600, 480)).tobytes() != compose(
        image, size=(800, 480)).crop((0, 445, 600, 480)).tobytes()


def lcd_sample(sample, **changes):
    from ventanal.devices.lcd_weather import render_lcd_weather
    w, alerts, radar, lines, place, coords, now, config = sample
    return render_lcd_weather(w, alerts, radar, replace(config, **changes), now,
                              lines=lines, place=place, coords=coords)


def test_lcd_weather_is_native_full_screen(sample):
    from ventanal.devices.lcd_lcars import CLOCK_GAP, LCD_COLOURS, TOPBAR_H
    assert TOPBAR_H == 26
    assert CLOCK_GAP == (548, 0, 204, 26)
    image = lcd_sample(sample)
    assert image.size == (800, 480) and image.mode == 'RGB'
    from PIL import ImageColor
    assert image.getpixel((10, 170)) == ImageColor.getrgb(LCD_COLOURS['orange'])
    x, y, w, h = CLOCK_GAP
    assert not np.asarray(image.crop((x, y, x+w, y+h))).any()
    assert image.getpixel((770, 13)) == ImageColor.getrgb('#ddbbff')


def test_lcd_weather_golden(sample):
    image = lcd_sample(sample)
    with Image.open(Path(__file__).parent / 'golden/lcd43b/weather.png') as expected:
        assert image.tobytes() == expected.convert('RGB').tobytes()


def test_lcd_weather_no_overlap(sample):
    from ventanal.devices.lcd_weather import weather_layout
    from ventanal.weather.data import Alert
    w, _, _, _, _, _, now, config = sample
    alert = Alert('Long advisory headline ' * 18, '', 'Minor', None)
    layout = weather_layout([alert], config, now)
    assert len(layout['advisory_lines']) == 3
    fx0, fy0, fx1, fy1 = layout['forecast_bbox']
    ax0, ay0, ax1, ay1 = layout['advisory_bbox']
    assert fy1 < ay0 < ay1 < 458
    assert 140 <= ax0 < ax1 <= 786
    assert len(w.days) == 7
    args = list(sample)
    args[1] = [alert]
    assert lcd_sample(args).size == (800, 480)


@pytest.mark.parametrize('clock_24h', [True, False])
def test_device_manifest_has_clock_24h(tmp_path, monkeypatch, clock_24h):
    value = cfg(tmp_path, clock_24h=clock_24h)
    run_device_tick(monkeypatch, value, screens={})
    assert read_manifest(lcd43b.device_dir(value))['clock_24h'] is clock_24h


def run_device_tick(monkeypatch, cfg_value, *, screens, solar=False, spares=(), weather_failure=False):
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
    from ventanal.art.sources import Candidate
    monkeypatch.setattr(jobs, 'ArtPool', lambda path: type('Pool', (), {
        'get': staticmethod(lambda source, object_id: (
            Candidate(source, object_id, '', '', '', '', '', None, None)
            if source else None)),
    })())
    monkeypatch.setattr(jobs, 'fetch_candidate',
                        lambda candidate, client, c: art_image)
    def weather_frame(*args):
        if weather_failure:
            raise RuntimeError('offline')
        return Image.new('RGB', (800, 480), (0, 255, 0))

    monkeypatch.setattr(lcd43b, 'weather_frame', weather_frame)
    if not solar:
        monkeypatch.setattr(devices.lcd43b, '_solar_allowed',
                            lambda c, n: False)
    else:
        monkeypatch.setattr(devices.lcd43b, '_solar_allowed',
                            lambda c, n: True)
        from ventanal.solar import data as solar_data
        monkeypatch.setattr(solar_data, 'fetch_solar',
                            lambda client, cache_dir, now: object())
        from ventanal.devices import lcd_solar
        monkeypatch.setattr(lcd_solar, 'render_lcd_solar',
                            lambda *a: Image.new('RGB', (800, 480), (0, 0, 255)))
        from ventanal.weather import data as weather_data
        monkeypatch.setattr(weather_data, 'geocode',
                            lambda *a: (LAT, LON, PLACE))
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
        assert entry['encoding'] == 'zlib'
        assert entry['size'] == len(data) < lcd43b.FRAME_SIZE
        assert len(zlib.decompress(data)) == lcd43b.FRAME_SIZE
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
    assert entry['encoding'] == 'zlib'
    assert zlib.decompress(data).startswith(b'VNR1')
    assert entry['sha256'] == hashlib.sha256(data).hexdigest()


def test_failed_screen_keeps_previous_device_entry(tmp_path, monkeypatch):
    value = cfg(tmp_path)
    run_device_tick(monkeypatch, value,
                    screens={'art': manifest_for(value, 'art'),
                             'weather': manifest_for(value, 'weather')})
    root = value.output_dir / 'devices' / 'lcd43b'
    previous = json.loads((root / 'manifest.json').read_text())

    run_device_tick(monkeypatch, value, screens={'art': manifest_for(value, 'art')},
                    weather_failure=True)
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
    assert load_config(EXAMPLE).device_id == 'lcd43b'
    path = tmp_path / 'enabled.toml'
    path.write_text(EXAMPLE.read_text().replace('lcd43b = false',
                                                 'lcd43b = true'))
    assert load_config(path).lcd43b is True
    bad = tmp_path / 'bad.toml'
    bad.write_text(EXAMPLE.read_text().replace('lcd43b = false',
                                               'lcd43b = "yes"'))
    with pytest.raises(ConfigError, match='devices.lcd43b'):
        load_config(bad)
    bad_id = tmp_path / 'bad-id.toml'
    bad_id.write_text(EXAMPLE.read_text().replace(
        'lcd43b = false', 'lcd43b = false\nid = "Not A Slug"'))
    with pytest.raises(ConfigError, match='devices.id'):
        load_config(bad_id)


def test_device_id_configures_publish_path(tmp_path, monkeypatch):
    value = cfg(tmp_path, device_id='lcd43b-1')
    run_device_tick(monkeypatch, value,
                    screens={'art': manifest_for(value, 'art'),
                             'weather': manifest_for(value, 'weather')})
    manifest = json.loads((value.output_dir / 'devices' / 'lcd43b-1'
                          / 'manifest.json').read_text())
    assert manifest['screens']['art']['url'].startswith(
        '/devices/lcd43b-1/frames/')
    assert (value.output_dir / 'devices' / 'lcd43b-1' / 'frames').is_dir()
    assert not (value.output_dir / 'devices' / 'lcd43b').exists()


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
        monkeypatch.setattr(jobs, 'geocode', lambda *a: (LAT, LON, PLACE))
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


def test_epaper_outputs_unchanged_after_a_successful_publish(tmp_path, monkeypatch):
    """A successful lcd43b publish must not perturb any e-paper artifact.

    The same deterministic tick runs with the profile off and on (a fully
    successful device publish, not the failure-isolation case above);
    every artifact outside the device subtree must stay byte-identical.
    """

    from ventanal import jobs
    from ventanal.art.sources import Candidate

    def snapshots(enabled):
        work = tmp_path / ('successful-on' if enabled else 'successful-off')
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
        monkeypatch.setattr(jobs, 'geocode', lambda *a: (LAT, LON, PLACE))
        monkeypatch.setattr(jobs, 'fetch_weather', lambda *a, **k: object())
        monkeypatch.setattr(jobs, 'fetch_alerts', lambda *a: [])
        monkeypatch.setattr(jobs, 'fetch_iem', lambda *a: None)
        monkeypatch.setattr(jobs, 'basemap', lambda *a: {'states': [], 'counties': []})
        monkeypatch.setattr(jobs, 'render_weather',
                            lambda *a: solid_canvas((8, 8, 8)))
        monkeypatch.setattr(jobs, 'fetch_candidate',
                            lambda candidate, client, c: Image.new('RGB', (16, 12)))
        monkeypatch.setattr(lcd43b, 'weather_frame',
                            lambda *a: Image.new('RGB', (800, 480)))
        with httpx.Client() as client:
            jobs.tick(value, NOW, client)
        e_paper = {}
        device = {}
        for path in sorted((value.output_dir).rglob('*')):
            if path.is_file():
                key = str(path.relative_to(value.output_dir))
                (device if 'devices' in path.parts else e_paper)[key] = \
                    path.read_bytes()
        return e_paper, device

    baseline, _ = snapshots(False)
    e_paper, device = snapshots(True)
    assert set(baseline) == set(e_paper)
    for name, data in baseline.items():
        assert e_paper[name] == data, name
    # The device subtree really was published and is complete.
    assert 'devices/lcd43b/manifest.json' in device
    manifest = json.loads(device['devices/lcd43b/manifest.json'])
    assert set(manifest['screens']) == {'art', 'weather'}
    for entry in manifest['screens'].values():
        data = device[f"devices/lcd43b/frames/"
                      f"{entry['url'].rsplit('/', 1)[1]}"]
        assert entry['encoding'] == 'zlib'
        assert entry['size'] == len(data) < lcd43b.FRAME_SIZE
        assert len(zlib.decompress(data)) == lcd43b.FRAME_SIZE
        assert entry['sha256'] == hashlib.sha256(data).hexdigest()


def test_device_manifest_request_time_solar_gate(tmp_path, monkeypatch):
    """Device manifests must re-check the ham licence on every request.

    Solar may be published while authorised and then go stale (licence
    expiry, call sign removed): /devices/<id>/manifest.json must stop
    advertising it at request time, fail closed, while the file on disk
    keeps the published entry for when the licence is valid again.
    """
    from ventanal.config import load_config
    from ventanal.solar.licence import Verification, verify

    root = tmp_path / 'out'
    device = root / 'devices' / 'lcd43b'
    (device / 'frames').mkdir(parents=True)
    (root / 'manifest.json').write_text('{}')
    solar = {'id': 'c' * 16, 'url': '/devices/lcd43b/frames/solar.vnr',
             'size': 768016, 'sha256': '3' * 64}
    manifest = {
        'version': 1,
        'screens': {
            'art': {'id': 'a' * 16, 'url': '/devices/lcd43b/frames/art.vnr',
                    'size': 768016, 'sha256': '1' * 64},
            'weather': {'id': 'b' * 16, 'url': '/devices/lcd43b/frames/weather.vnr',
                        'size': 768016, 'sha256': '2' * 64},
            'solar': solar,
        },
        'quiet': {'start': '23:00', 'end': '06:00'},
    }
    (device / 'manifest.json').write_text(json.dumps(manifest))

    config_path = tmp_path / 'ventanal.toml'
    config_path.write_text(
        EXAMPLE.read_text()
        .replace('/var/lib/ventanal', str(tmp_path / 'lib'))
        .replace('/var/cache/ventanal', str(tmp_path / 'cache'))
        .replace('lcd43b = false', 'lcd43b = true')
        .replace('call_sign = ""', 'call_sign = "W1AW"'))
    cfg = load_config(config_path)
    cfg.cache_dir.mkdir(parents=True, exist_ok=True)

    def request():
        srv = make_server(root, '127.0.0.1', 0, cache_dir=cfg.cache_dir,
                          config_path=config_path)
        thread = Thread(target=srv.serve_forever, daemon=True)
        thread.start()
        try:
            conn = HTTPConnection(*srv.server_address)
            conn.request('GET', '/devices/lcd43b/manifest.json')
            response = conn.getresponse()
            body = json.loads(response.read())
            conn.close()
            return body
        finally:
            srv.shutdown()
            srv.server_close()
            thread.join()

    # Licence valid: solar is advertised.
    monkeypatch.setattr('ventanal.solar.licence.verify',
                        lambda *a, **k: Verification(True, 'ok'))
    assert 'solar' in request()['screens']
    # Licence expired: solar must be filtered at request time.
    monkeypatch.setattr('ventanal.solar.licence.verify',
                        lambda *a, **k: Verification(False, 'expired'))
    assert 'solar' not in request()['screens']
    # Call sign removed from the configuration: fail closed.
    config_path.write_text(config_path.read_text()
                           .replace('call_sign = "W1AW"', 'call_sign = ""'))
    assert 'solar' not in request()['screens']
    # The published file itself is untouched for when the licence returns.
    assert 'solar' in json.loads(
        (device / 'manifest.json').read_text())['screens']
    # Licence valid again (call sign restored): solar is advertised once more.
    config_path.write_text(config_path.read_text()
                           .replace('call_sign = ""', 'call_sign = "W1AW"'))
    monkeypatch.setattr('ventanal.solar.licence.verify',
                        lambda *a, **k: Verification(True, 'ok'))
    assert 'solar' in request()['screens']


def test_device_manifest_symlink_rejected(tmp_path):
    """A configured server must apply the shared path-resolution/symlink
    check before opening device manifests.

    A symlinked devices/<id>/manifest.json — or a symlinked devices/<id>
    parent directory — must never serve a file that resolves outside the
    publication root: 404, fail closed, like every other route.
    """
    root = tmp_path / 'out'
    (root / 'devices').mkdir(parents=True)
    (root / 'manifest.json').write_text('{}')
    secret = tmp_path / 'secret.json'
    secret.write_text('{"version": 1, "screens": {}, "leaked": true}')

    config_path = tmp_path / 'ventanal.toml'
    config_path.write_text(
        EXAMPLE.read_text()
        .replace('/var/lib/ventanal', str(tmp_path / 'lib'))
        .replace('/var/cache/ventanal', str(tmp_path / 'cache'))
        .replace('lcd43b = false', 'lcd43b = true'))
    cfg = load_config(config_path)
    cfg.cache_dir.mkdir(parents=True, exist_ok=True)

    def request(path):
        srv = make_server(root, '127.0.0.1', 0, cache_dir=cfg.cache_dir,
                          config_path=config_path)
        thread = Thread(target=srv.serve_forever, daemon=True)
        thread.start()
        try:
            conn = HTTPConnection(*srv.server_address)
            conn.request('GET', path)
            response = conn.getresponse()
            body = response.read()
            conn.close()
            return response.status, body
        finally:
            srv.shutdown()
            srv.server_close()
            thread.join()

    # Positive control: a real manifest under the publication root serves.
    device = root / 'devices' / 'lcd43b'
    device.mkdir()
    (device / 'manifest.json').write_text('{"version": 1}')
    status, _ = request('/devices/lcd43b/manifest.json')
    assert status == 200

    # Symlinked manifest.json pointing outside the publication root.
    (device / 'manifest.json').unlink()
    (device / 'manifest.json').symlink_to(secret)
    status, body = request('/devices/lcd43b/manifest.json')
    assert status == 404
    assert b'leaked' not in body

    # Symlinked parent directory: devices/<id> itself resolves outside.
    (device / 'manifest.json').unlink()
    device.rmdir()
    (tmp_path / 'beyond').mkdir()
    (tmp_path / 'beyond' / 'manifest.json').symlink_to(secret)
    (root / 'devices' / 'elsewhere').symlink_to(tmp_path / 'beyond',
                                                target_is_directory=True)
    status, body = request('/devices/elsewhere/manifest.json')
    assert status == 404
    assert b'leaked' not in body


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

@pytest.mark.parametrize('label', ['off', 'en', 'es'])
def test_lcd_weather_location_label(sample, monkeypatch, label):
    from ventanal.devices import lcd_weather
    calls = []
    original = lcd_weather.location_marker

    def marker(canvas, centre, here_label):
        calls.append(here_label)
        return original(canvas, centre, here_label)

    monkeypatch.setattr(lcd_weather, 'location_marker', marker)
    lcd_sample(sample, here_label=label)
    assert calls == [label]


@pytest.mark.parametrize('missing', [None, 'alerts', 'radar', 'basemap'])
def test_lcd_weather_data_pipeline(sample, monkeypatch, caplog, missing):
    from ventanal import jobs
    from ventanal.devices import lcd_weather
    w, alerts, radar, lines, place, coords, now, config = sample
    calls = []

    def provider(name, result):
        def fetch(*args, **kwargs):
            calls.append(name)
            if name == missing:
                raise RuntimeError('unavailable')
            return result
        return fetch

    monkeypatch.setattr(jobs, 'geocode', provider('geocode', (*coords, place)))
    monkeypatch.setattr(jobs, 'fetch_weather', provider('weather', w))
    monkeypatch.setattr(jobs, 'fetch_alerts', provider('alerts', alerts))
    monkeypatch.setattr(jobs, 'fetch_iem', provider('radar', radar))
    monkeypatch.setattr(jobs, 'basemap', provider('basemap', lines))
    image = lcd_weather.weather_frame(replace(config, radar='iem'), now, object())
    assert calls == ['geocode', 'weather', 'alerts', 'radar', 'basemap']
    expected = lcd_weather.render_lcd_weather(
        w, [] if missing == 'alerts' else alerts,
        None if missing == 'radar' else radar, config, now,
        lines={} if missing == 'basemap' else lines, place=place, coords=coords)
    assert image.tobytes() == expected.tobytes()
    if missing:
        assert f'LCD weather {missing} unavailable: unavailable' in caplog.text


def test_lcd_chrome_has_aa_edges_without_clock_bleed():
    from ventanal.devices.lcd_lcars import draw_lcd_frame
    image = Image.new('RGB', (800, 480))
    draw_lcd_frame(image, [])
    pixels = np.asarray(image)
    edge = pixels[:44, :44, 0]
    assert ((edge > 0) & (edge < 255)).any()
    assert not pixels[:26, 548:752].any()


def test_lcd_text_edges_have_intermediate_colours(sample, tmp_path):
    for image in (lcd_sample(sample), solar_sample(tmp_path)):
        colours = np.asarray(image)[30:58, 150:500].reshape(-1, 3)
        assert any(0 < int(r) < 255 for r, g, b in colours)


@pytest.mark.parametrize('page', ['weather', 'solar'])
def test_lcd_epaper_palette_and_sidebar_labels(sample, tmp_path, monkeypatch, page):
    from PIL import ImageColor
    from PIL import ImageDraw
    from ventanal.palette import LCARS_MODERN

    labels = []
    original = ImageDraw.ImageDraw.text

    def capture(draw, xy, text, *args, **kwargs):
        if xy[0] == 92:
            labels.append((text, kwargs['fill']))
        return original(draw, xy, text, *args, **kwargs)

    monkeypatch.setattr(ImageDraw.ImageDraw, 'text', capture)
    if page == 'weather':
        # Codes must follow the data, independently of the golden's timestamp.
        w, alerts, radar, lines, place, coords, now, config = sample
        args = (replace(w, sunrise=w.sunrise.replace(minute=23)), alerts,
                radar, lines, place, coords, now.replace(hour=9, minute=7), config)
        image = lcd_sample(args)
        bands = [(80, 'sky'), (170, 'orange'), (250, 'light_gray'), (350, 'gray')]
        assert labels == [('319-0907', 'black'), ('RADAR', 'black'),
                          ('0723-1716', 'black'), ('LONG RANGE', 'white')]
        accent, block = 'violet', 'orange'
    else:
        image = solar_sample(tmp_path)
        bands = [(80, 'sky'), (150, 'red'), (250, 'gold'),
                 (350, 'sky'), (400, 'violet')]
        assert labels == [(label, 'black') for label in
                          ('SOLAR FLUX', 'GEOMAG', 'HF BANDS', 'KP 24 H', '3-DAY')]
        accent, block = 'orange', 'orange'
    for y, colour in bands:
        assert image.getpixel((10, y)) == ImageColor.getrgb(LCARS_MODERN[colour])
    for xy in ((200, 13), (10, 40), (200, 470), (770, 13)):
        assert image.getpixel(xy) == ImageColor.getrgb(LCARS_MODERN[accent])
    assert image.getpixel((540, 13)) == ImageColor.getrgb(LCARS_MODERN[block])
    assert not np.asarray(image)[:26, 548:752].any()
