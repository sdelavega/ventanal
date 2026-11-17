# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Stephen de la Vega

from dataclasses import replace
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFilter
import pytest

from ventanal.art.compose import compose
from ventanal.art.render import ART_READOUT, render_art
from ventanal.art.sources import Candidate
from ventanal.config import load_config
from ventanal.manifest import Readout
from ventanal.palette import to_oklab
from ventanal.quantize import Canvas, Method, quantize, simulate
from ventanal.text import fit, font, renderable

ROOT = Path(__file__).resolve().parents[2]
CANDIDATE = Candidate('test', '1', 'Hunters in the Snow', 'Pieter Bruegel the Elder',
                      '1565', 'Kunsthistorisches Museum', '', None, None)


def test_crop_vs_fit():
    landscape = Image.new('RGB', (2000, 1500), (120, 160, 200))
    assert compose(landscape).getpixel((0, 0)) == landscape.getpixel((0, 0))
    portrait = Image.new('RGB', (1000, 2000), (120, 160, 200))
    result = compose(portrait)
    assert np.mean(result.getpixel((0, 600))) < np.asarray(portrait).mean()
    assert result.getpixel((800, 600)) == portrait.getpixel((0, 0))


def test_soft_shade_matches_mock():
    assert ART_READOUT == Readout(1020, 0, 580, 104, 'shade')
    backdrop = Image.new('RGB', (1600, 1200), (120, 160, 200))
    canvas, credit = render_art(backdrop, CANDIDATE)
    ft, fs = font('inter-bold', 17), font('inter', 15)
    assert credit['title'] == CANDIDATE.title
    assert credit['secondary'] == '   Pieter Bruegel the Elder · 1565 · Kunsthistorisches Museum'
    text_width = ft.getlength(credit['title']) + fs.getlength(credit['secondary'])
    mask = Image.new('L', backdrop.size)
    md = ImageDraw.Draw(mask)
    md.rounded_rectangle((0, 1138, 28 + text_width + 40, 1200), 30, fill=150)
    expected = Canvas()
    expected.paste(backdrop, (0, 0), Method.DIFFUSE)
    expected.paste(Image.new('RGB', backdrop.size), (0, 0), Method.DIFFUSE,
                   mask.filter(ImageFilter.GaussianBlur(14)))
    draw = expected.region(Method.SOLID)
    draw.text((28, 1172), credit['title'], font=ft, fill='white', anchor='ls')
    draw.text((28 + ft.getlength(credit['title']), 1172), credit['secondary'],
              font=fs, fill='white', anchor='ls')
    np.testing.assert_array_equal(canvas.rgb, expected.rgb)
    np.testing.assert_array_equal(canvas.method, expected.method)
    np.testing.assert_array_equal(np.asarray(canvas.rgb)[:104], np.asarray(backdrop)[:104])
    # Top artwork is untouched regardless of credit length.
    other, _ = render_art(backdrop, replace(CANDIDATE, title='W' * 300))
    np.testing.assert_array_equal(np.asarray(canvas.rgb)[:100], np.asarray(other.rgb)[:100])


def test_credit_long_title():
    canvas, credit = render_art(Image.new('RGB', (1600, 1200)),
                                replace(CANDIDATE, title='W' * 300))
    assert credit['x'] == 28 and credit['y'] == 1172
    assert credit['w'] <= 1544
    assert credit['title'].endswith('…')
    assert font('inter-bold', 17).getlength(credit['title']) <= 1544 * .6
    assert (font('inter-bold', 17).getlength(credit['title'])
            + font('inter', 15).getlength(credit['secondary'])) <= 1544
    assert np.any(np.asarray(canvas.method)[1138:1200] == Method.SOLID)


@pytest.mark.parametrize('title,expected', [('富嶽三十六景 Great Wave', 'Great Wave'),
                                            ('漢字', 'Untitled'), ('   ', 'Untitled')])
def test_credit_missing_glyphs(title, expected):
    _, credit = render_art(Image.new('RGB', (1600, 1200)), replace(CANDIDATE, title=title))
    assert credit['title'] == expected


def test_text_helpers():
    for name in ('antonio', 'antonio-bold', 'inter', 'inter-bold'):
        f = font(name, 30)
        assert renderable('漢字  Hello\n world', name) == 'Hello world'
        assert fit('Hello', f, 1000) == 'Hello'
        assert fit('Hello world' * 20, f, 100).endswith('…')
        assert f.getlength(fit('Hello world' * 20, f, 100)) <= 100
        assert fit('Hello', f, 0) == ''


def test_credit_ellipsis_has_no_trailing_separator():
    f = font('inter', 15)
    assert fit('Artist · Museum', f, f.getlength('Artist · …')) == 'Artist…'


def test_golden_art():
    panel = load_config(ROOT / 'server/ventanal.example.toml').panel_rgb
    with Image.open(ROOT / 'docs/mockups/hunters.jpg') as image:
        canvas, _ = render_art(image, CANDIDATE)
    actual = simulate(quantize(canvas, panel), panel)
    # Independent colour-fidelity gate: beat measured plain RGB FS (0.01439691),
    # even when a changed golden would otherwise approve its own regression.
    reference = to_oklab(np.asarray(canvas.rgb.filter(ImageFilter.GaussianBlur(2))))
    viewed = to_oklab(np.asarray(actual.filter(ImageFilter.GaussianBlur(2))))
    assert np.linalg.norm(viewed - reference, axis=-1).mean() < 0.01439
    golden = Path(__file__).parent / 'golden/art.png'
    if not golden.exists():
        golden.parent.mkdir(parents=True, exist_ok=True)
        actual.save(golden)
        pytest.skip('Generated golden/art.png; requires human visual approval')
    with Image.open(golden) as expected:
        assert actual.size == expected.size
        differing = np.any(np.asarray(actual) != np.asarray(expected.convert('RGB')), axis=2)
    assert differing.mean() <= 0.005


def test_live_long_title_preserves_artist():
    _, credit = render_art(Image.new('RGB', (1600, 1200)), replace(
        CANDIDATE,
        title='Shimla. View of the Church from Mount Jakko Looking West with Part of the Mall in Winter',
        artist='Samuel Bourne', date='1863–1866', museum='Cleveland Museum of Art'))
    assert credit['title'].endswith('Winter')
    assert font('inter-bold', 17).getlength(credit['title']) <= 1544 * .6
    assert 'Samuel Bourne' in credit['secondary']


@pytest.mark.parametrize(('artist', 'date', 'museum', 'expected'), [
    ('Artist', '1900', 'M' * 200, '   Artist · 1900'),
    ('Artist', 'D' * 200, 'Museum', '   Artist'),
    ('A' * 200, '1900', 'Museum', None),
])
def test_credit_secondary_drops_fields_before_artist(artist, date, museum, expected):
    _, credit = render_art(Image.new('RGB', (1600, 1200)), replace(
        CANDIDATE, title='W' * 300, artist=artist, date=date, museum=museum))
    if expected is None:
        assert credit['secondary'].endswith('…') and '·' not in credit['secondary']
    else:
        assert credit['secondary'] == expected
