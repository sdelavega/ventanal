# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Stephen de la Vega

from dataclasses import replace
from datetime import datetime, timedelta
from pathlib import Path
from zoneinfo import ZoneInfo

import numpy as np
from PIL import Image, ImageDraw
import pytest

from ventanal.config import load_config
from ventanal.manifest import Readout
from ventanal.palette import PANEL_INDEX
from ventanal.quantize import Canvas, Method, quantize, simulate
from ventanal.text import renderable
from ventanal.weather.data import Alert, Current, Day, Weather
from ventanal.weather.radar import Radar
from ventanal.weather.render import WEATHER_READOUT, render_weather

ROOT = Path(__file__).resolve().parents[2]
NOW = datetime(2026, 11, 15, 14, 32, tzinfo=ZoneInfo('America/New_York'))


@pytest.fixture
def sample():
    """The approved mockup's seven temperatures, probabilities and radar blobs."""
    cfg = replace(load_config(ROOT / 'server/ventanal.example.toml'),
                  zip='80302', clock_24h=True, units='imperial', here_label='es')
    values = [(61, 44, 61, 40), (55, 38, 61, 80), (49, 31, 0, 0),
              (52, 33, 0, 5), (60, 40, 3, 10), (63, 46, 80, 30), (57, 39, 3, 15)]
    weather = Weather(Current(58, 62, 8, 'NE', 2),
                      [Day((NOW + timedelta(days=i)).date(), *v) for i, v in enumerate(values)],
                      NOW.replace(hour=7, minute=11), NOW.replace(hour=17, minute=16),
                      NOW.replace(hour=18, minute=0), NOW)
    yy, xx = np.mgrid[234:696, 784:1566]
    levels = np.zeros((462, 782), dtype=np.uint8)
    for x, y, rx, ry, level in [(1260, 430, 190, 105, 1), (1270, 425, 120, 62, 2),
                               (1285, 418, 55, 30, 3), (930, 600, 90, 48, 1)]:
        dx, dy = xx - x, yy - y
        u = dx * np.cos(-.4) + dy * np.sin(-.4)
        v = -dx * np.sin(-.4) + dy * np.cos(-.4)
        levels[(u / rx) ** 2 + (v / ry) ** 2 <= 1] = level
    radar = Radar(levels, NOW.replace(minute=25))
    lines = [[(x - 784, 0), (x - 784, 461)] for x in range(860, 1570, 80)]
    lines += [[(0, y - 234), (781, y - 234)] for y in range(310, 696, 80)]
    alerts = [Alert('Frost likely Tue–Wed nights', '', 'Minor', None)]
    return weather, alerts, radar, {'states': lines[:2], 'counties': lines[2:]}, 'BOULDER · CO', (40.01, -105.27), NOW, cfg


def test_readout_block_reserved(sample):
    assert WEATHER_READOUT == Readout(1236, 30, 334, 136, 'lcars')
    canvas = render_weather(*sample)
    assert canvas.rgb.getpixel((0, 0)) == (0, 0, 0)
    r = WEATHER_READOUT
    assert not np.any(np.asarray(canvas.method)[r.y:r.y+r.h, r.x:r.x+r.w] == Method.SOLID)


def test_text_is_solid(sample):
    canvas = render_weather(*sample)
    methods = np.asarray(canvas.method)[270:430, 330:540]
    rgb = np.asarray(canvas.rgb)[270:430, 330:540]
    white = np.all(rgb == sample[-1].panel_rgb[PANEL_INDEX['white']], axis=2)
    assert white.sum() > 4000
    assert np.all(methods[white] == Method.SOLID)
    # Every SOLID pixel is an exact panel colour, including glyph edges.
    solid = np.asarray(canvas.rgb)[np.asarray(canvas.method) == Method.SOLID]
    assert np.all(np.any(np.all(solid[:, None] == sample[-1].panel_rgb, axis=2), axis=1))


def test_no_alerts_line(sample):
    args = list(sample)
    args[1] = []
    rgb = np.asarray(render_weather(*args).rgb)[1020:1071, 330:1570]
    assert np.any(np.all(rgb == args[-1].panel_rgb[1], axis=2))
    assert not np.any(np.all(rgb == args[-1].panel_rgb[3], axis=2))


def test_radar_unavailable(sample):
    args = list(sample)
    args[2] = None
    canvas = render_weather(*args)
    red = np.all(np.asarray(canvas.rgb)[234:696, 784:1566] == args[-1].panel_rgb[3], axis=2)
    assert red.sum() > 100
    assert np.all(np.asarray(canvas.method)[234:696, 784:1566][red] == Method.SOLID)


def test_radar_inner_origin_and_clear_pixels(sample):
    args = list(sample)
    levels = np.zeros((462, 782), dtype=np.uint8)
    for x, level in ((10, 1), (30, 2), (50, 3)):
        levels[36:44, x:x+8] = level
    args[2] = Radar(levels, NOW)
    args[3] = {'states': [], 'counties': [[(5, 50), (10, 50)]]}
    canvas = render_weather(*args)
    for offset, name in enumerate(('green', 'yellow', 'red')):
        assert canvas.rgb.getpixel((796 + offset * 20, 274)) == args[-1].panel_rgb[PANEL_INDEX[name]]
        assert canvas.method.getpixel((796 + offset * 20, 274)) == Method.SOLID
    assert canvas.method.getpixel((830, 280)) == Method.SOLID
    assert canvas.rgb.getpixel((790, 284)) == args[-1].panel_rgb[PANEL_INDEX['blue']]
    assert canvas.method.getpixel((783, 234)) == Method.PATTERN


def test_time_and_location_text(sample, monkeypatch):
    import ventanal.weather.render as module
    texts = []
    original = module._text

    def capture(canvas, text, *args, **kwargs):
        texts.append(text)
        return original(canvas, text, *args, **kwargs)

    monkeypatch.setattr(module, '_text', capture)
    args = list(sample)
    args[1] = [Alert('Freeze warning', '', 'Severe', NOW.replace(hour=23, minute=0))]
    args[-1] = replace(args[-1], clock_24h=False, units='metric')
    render_weather(*args)
    assert 'BOULDER · CO · 80302 · 40.01N 105.27W' in texts
    assert 'WIND NE 8 KM/H' in texts
    assert 'RAIN 40% AFTER 6:00 PM' in texts
    assert 'SUNSET 5:16 PM' in texts
    assert 'NEXRAD COMPOSITE · 2:25 PM · 150 MI' in texts
    assert 'ADVISORY · FREEZE WARNING UNTIL Sun 11:00 PM' in texts
    assert '319-1432' in texts
    assert '0711-1716' in texts


def test_location_caption_above_marker(sample, monkeypatch):
    import ventanal.weather.render as module
    captions = []
    original = module._text

    def capture(canvas, text, xy, px, colour, **kwargs):
        if text == 'ESTÁS AQUÍ':
            captions.append((px, kwargs.get('bold')))
        return original(canvas, text, xy, px, colour, **kwargs)

    assert renderable('ESTÁS AQUÍ', 'antonio-bold') == 'ESTÁS AQUÍ'
    monkeypatch.setattr(module, '_text', capture)
    canvas = render_weather(*sample)
    rgb = np.asarray(canvas.rgb)
    methods = np.asarray(canvas.method)
    white = np.all(rgb[420:453, 1100:1260] == sample[-1].panel_rgb[1], axis=2)
    assert white.sum() > 200
    assert np.all(methods[420:453, 1100:1260][white] == Method.SOLID)
    assert captions == [(26, True)]
    # The leader reaches the top of the radius-eight marker.
    assert np.all(rgb[459:461, 1180] == sample[-1].panel_rgb[0])
    assert np.all(methods[459:462, 1180] == Method.SOLID)


@pytest.mark.parametrize('label', ['en', 'es'])
@pytest.mark.parametrize('centre', [(396, 10), (10, 236), (771, 236), (10, 10), (771, 10)])
def test_location_caption_clamped(sample, monkeypatch, centre, label):
    import ventanal.weather.render as module
    from ventanal.text import font

    canvas = Canvas((782, 462), fill=sample[-1].panel_rgb[3])
    colours = {name: tuple(sample[-1].panel_rgb[index]) for name, index in PANEL_INDEX.items()}
    original = module._text
    bounds = []

    def capture(canvas, text, xy, px, colour, **kwargs):
        left, top, right, bottom = font('antonio-bold', px).getbbox(text, anchor='ls')
        bounds.append((xy[0] + left, xy[1] + top, xy[0] + right, xy[1] + bottom))
        return original(canvas, text, xy, px, colour, **kwargs)

    monkeypatch.setattr(module, '_text', capture)
    module._location_marker(canvas, centre, colours, label)
    assert len(bounds) == 1
    left, top, right, bottom = bounds[0]
    assert 6 <= left < right <= 776
    assert 6 <= top < bottom <= 456
    if centre[1] == 10:
        assert top - 6 > centre[1] + 9
    else:
        assert bottom + 6 < centre[1] - 9
    rgb = np.asarray(canvas.rgb)
    methods = np.asarray(canvas.method)
    glyphs = np.all(rgb[top:bottom, left:right] == colours['white'], axis=2)
    assert glyphs.sum() > 200
    assert np.all(methods[top:bottom, left:right][glyphs] == Method.SOLID)
    # Check backing padding beside the leader's attachment point.
    padding_x = (left + right) // 2 + 10
    assert np.all(rgb[bottom:bottom + 6, padding_x] == colours['black'])
    assert np.all(methods[bottom:bottom + 6, padding_x] == Method.SOLID)


def test_location_caption_requires_accents(sample, monkeypatch):
    import ventanal.weather.render as module
    monkeypatch.setattr(module, 'renderable', lambda text, name: text.replace('Á', ''))
    with pytest.raises(ValueError, match='including Á and Í'):
        render_weather(*sample)


@pytest.mark.parametrize(('onset', 'days', 'expected'), [
    (None, True, 'NO RAIN EXPECTED TODAY'),
    (NOW.replace(hour=18, minute=0), False, 'RAIN AFTER 18:00'),
    (NOW.replace(hour=18, minute=0), True, 'RAIN 40% AFTER 18:00'),
])
def test_rain_line(sample, monkeypatch, onset, days, expected):
    import ventanal.weather.render as module
    texts = []
    original = module._text

    def capture(canvas, text, *args, **kwargs):
        texts.append(text)
        return original(canvas, text, *args, **kwargs)

    monkeypatch.setattr(module, '_text', capture)
    args = list(sample)
    args[0] = replace(args[0], rain_onset=onset, days=args[0].days if days else [])
    render_weather(*args)
    assert expected in texts


def test_radar_shape_validated_before_map_lines(sample):
    def invalid_lines():
        raise AssertionError('map lines consumed before radar validation')
        yield

    args = list(sample)
    args[2] = Radar(np.zeros((1, 1), dtype=np.uint8), NOW)
    args[3] = invalid_lines()
    with pytest.raises(ValueError, match='radar.levels must have shape'):
        render_weather(*args)


def test_golden_weather(sample):
    canvas = render_weather(*sample)
    actual = simulate(quantize(canvas, sample[-1].panel_rgb), sample[-1].panel_rgb)
    golden = Path(__file__).parent / 'golden/weather.png'
    if not golden.exists():
        golden.parent.mkdir(parents=True, exist_ok=True)
        actual.save(golden)
        pytest.skip('Generated golden/weather.png; requires controller visual approval')
    with Image.open(golden) as expected:
        assert actual.size == expected.size
        differing = np.any(np.asarray(actual) != np.asarray(expected.convert('RGB')), axis=2)
    assert differing.mean() <= 0.005


def test_map_layer_stroke_widths(sample):
    args = list(sample)
    args[2] = Radar(np.zeros((462, 782), dtype=np.uint8), NOW)
    args[3] = {'counties': [[(10, 10), (30, 10)]],
               'states': [[(10, 30), (30, 30)]]}
    rgb = np.asarray(render_weather(*args).rgb)
    blue = args[-1].panel_rgb[PANEL_INDEX['blue']]
    assert np.all(rgb[244, 804] == blue)
    assert not np.all(rgb[245, 804] == blue)
    assert np.all(rgb[264:266, 804] == args[-1].panel_rgb[0])
    assert np.all(rgb[266, 804] == args[-1].panel_rgb[1])

@pytest.mark.parametrize('label, expected', [('off', None), ('en', 'YOU ARE HERE'), ('es', 'ESTÁS AQUÍ')])
def test_caption_choices(sample, monkeypatch, label, expected):
    import ventanal.weather.render as module
    texts = []
    original = module._text
    def capture(canvas, text, *args, **kwargs):
        texts.append(text)
        return original(canvas, text, *args, **kwargs)
    monkeypatch.setattr(module, '_text', capture)
    args = list(sample)
    args[-1] = replace(args[-1], here_label=label)
    args[2] = Radar(np.zeros((462, 782), dtype=np.uint8), NOW)
    args[3] = {'states': [], 'counties': []}
    rgb = np.asarray(render_weather(*args).rgb)
    if expected:
        assert expected in texts
    else:
        assert 'YOU ARE HERE' not in texts and 'ESTÁS AQUÍ' not in texts
        # Exact dot-only raster in a region spanning the entire caption/leader.
        from PIL import ImageDraw
        dot = Image.new('RGB', (300, 110), tuple(args[-1].panel_rgb[1]))
        ImageDraw.Draw(dot).ellipse((142, 82, 158, 98), fill=tuple(args[-1].panel_rgb[1]))
        ImageDraw.Draw(dot).ellipse((143, 83, 157, 97), fill=tuple(args[-1].panel_rgb[0]))
        assert np.array_equal(rgb[380:490, 1030:1330], np.asarray(dot))
    assert np.all(rgb[470, 1180] == args[-1].panel_rgb[0])


@pytest.mark.parametrize('level', [1, 2, 3])
def test_echo_black_outline_and_white_map(sample, level):
    args = list(sample)
    levels = np.zeros((462, 782), dtype=np.uint8)
    levels[100:110, 100:110] = level
    args[2] = Radar(levels, NOW)
    args[3] = {'states': [], 'counties': []}
    canvas = render_weather(*args)
    rgb = np.asarray(canvas.rgb)[234:696, 784:1566]
    methods = np.asarray(canvas.method)[234:696, 784:1566]
    assert np.all(rgb[100:110, 98:100] == args[-1].panel_rgb[0])
    assert np.all(rgb[100:110, 110:112] == args[-1].panel_rgb[0])
    assert np.all(rgb[98:100, 98:112] == args[-1].panel_rgb[0])
    assert np.all(rgb[110:112, 98:112] == args[-1].panel_rgb[0])
    assert np.all(rgb[98:100, 98:112] == args[-1].panel_rgb[0])
    assert np.all(rgb[110:112, 98:112] == args[-1].panel_rgb[0])
    assert np.all(rgb[100:110, 100:110] == args[-1].panel_rgb[{1:5, 2:2, 3:3}[level]])
    assert np.all(methods[98:112, 98:112] == Method.SOLID)
    assert tuple(rgb[100, 97]) == args[-1].panel_rgb[1]


@pytest.mark.parametrize('unavailable', [False, True])
def test_rounded_inner_clips_rgb_and_methods(sample, unavailable):
    args = list(sample)
    args[2] = None if unavailable else Radar(np.full((462, 782), 2, dtype=np.uint8), NOW)
    args[3] = {'states': [[(-100, 0), (900, 0)]], 'counties': []}
    canvas = render_weather(*args)
    outer = Image.new('1', canvas.rgb.size)
    ImageDraw.Draw(outer).rounded_rectangle((780,230,1569,699), radius=28, fill=1)
    inner = Image.new('1', canvas.rgb.size)
    ImageDraw.Draw(inner).rounded_rectangle((784,234,1565,695), radius=24, fill=1)
    area = np.zeros((1200,1600), dtype=bool)
    area[230:700,780:1570] = True
    outside = area & ~np.asarray(outer)
    border = area & np.asarray(outer) & ~np.asarray(inner)
    rgb, methods = np.asarray(canvas.rgb), np.asarray(canvas.method)
    assert np.all(rgb[outside] == 0)
    assert np.all(methods[border | outside] == Method.PATTERN)
    from PIL import ImageColor
    from ventanal.palette import LCARS_MODERN
    assert np.all(rgb[border] == ImageColor.getrgb(LCARS_MODERN['sky']))
    if unavailable:
        assert tuple(rgb[300, 850]) == args[-1].panel_rgb[1]
    else:
        assert tuple(rgb[234, 810]) == args[-1].panel_rgb[2]


@pytest.mark.parametrize('label', ['off', 'es', 'en'])
@pytest.mark.parametrize('background', [0, 1, 3])
def test_smaller_black_marker_with_thin_white_ring(sample, label, background):
    import ventanal.weather.render as module
    colours = {name: tuple(sample[-1].panel_rgb[index]) for name, index in PANEL_INDEX.items()}
    canvas = Canvas((782, 462), fill=sample[-1].panel_rgb[background])
    module._location_marker(canvas, (396,236), colours, label)
    # Old total diameter 23 px; new 17 px is 26% smaller, black core 15 px.
    for dx in (-9, 9):
        assert canvas.rgb.getpixel((396+dx,236)) == sample[-1].panel_rgb[background]
    for dx in (-8, 8):
        assert canvas.rgb.getpixel((396+dx,236)) == colours['white']
    for dx in range(-7,8):
        assert canvas.rgb.getpixel((396+dx,236)) == colours['black']
        assert canvas.method.getpixel((396+dx,236)) == Method.SOLID


@pytest.mark.parametrize('level', [1, 2, 3])
def test_despeckle_before_outlining(sample, level):
    args = list(sample)
    levels = np.zeros((462, 782), dtype=np.uint8)
    levels[80, 80] = level
    levels[100:106, 100:108] = level  # 48 px: below the 50 px area floor.
    levels[140:150, 140:150] = level
    levels[139, 145] = level  # Opening removes a one-pixel protrusion.
    levels[145, 145] = 0  # Closing fills a one-pixel hole.
    args[2] = Radar(levels.copy(), NOW)
    args[3] = {'states': [], 'counties': []}
    canvas = render_weather(*args)
    rgb = np.asarray(canvas.rgb)[234:696, 784:1566]
    assert np.all(rgb[76:84, 76:84] == args[-1].panel_rgb[1])
    assert np.all(rgb[96:110, 96:112] == args[-1].panel_rgb[1])
    assert np.all(rgb[140:150, 140:150] == args[-1].panel_rgb[{1:5, 2:2, 3:3}[level]])
    assert np.all(rgb[138:140, 138:152] == args[-1].panel_rgb[0])
    assert np.array_equal(args[2].levels, levels)


def test_composite_caption_backing_covers_basemap(sample):
    args = list(sample)
    args[2] = Radar(np.zeros((462, 782), dtype=np.uint8), NOW)
    args[3] = {'counties': [[(0, y), (781, y)] for y in range(416, 462)], 'states': []}
    canvas = render_weather(*args)
    rgb = np.asarray(canvas.rgb)[234:696, 784:1566]
    # A dark rounded pill with white glyphs, isolated from black state lines.
    assert tuple(rgb[423, 30]) == args[-1].panel_rgb[0]
    assert np.count_nonzero(np.all(rgb[426:456, 20:310] == args[-1].panel_rgb[1], axis=2)) > 100
    assert tuple(rgb[420, 8]) == args[-1].panel_rgb[4]  # Rounded backing leaves its corner exposed.


@pytest.mark.parametrize('level', [1, 2, 3])
def test_small_echo_regions_removed_before_outline(sample, level):
    args = list(sample)
    levels = np.zeros((462, 782), dtype=np.uint8)
    levels[80, 80] = level
    levels[100:106, 100:108] = level  # 48 px: below the 50 px floor.
    levels[120:128, 120:128] = level  # 64 px: real compact cell survives.
    args[2] = Radar(levels, NOW)
    args[3] = {'states': [], 'counties': []}
    rgb = np.asarray(render_weather(*args).rgb)[234:696, 784:1566]
    assert np.all(rgb[75:112, 75:114] == args[-1].panel_rgb[1])
    assert np.all(rgb[120:128, 120:128] == args[-1].panel_rgb[{1:5,2:2,3:3}[level]])
    assert np.all(rgb[118:120, 118:130] == args[-1].panel_rgb[0])
