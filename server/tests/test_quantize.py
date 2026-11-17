# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Stephen de la Vega

from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFilter
import pytest

from ventanal.config import load_config
from ventanal.palette import LCARS_MODERN, PANEL_INDEX, to_oklab
from ventanal.quantize import Canvas, Method, quantize, simulate
import ventanal.quantize as quantizer


PANEL = load_config(Path(__file__).parents[1] / "ventanal.example.toml").panel_rgb


def test_solid_exact():
    canvas = Canvas((60, 10), method=Method.SOLID)
    for i, colour in enumerate(PANEL):
        canvas.region(Method.SOLID).rectangle((10*i, 0, 10*i+9, 9), fill=colour)
    indices = quantize(canvas, PANEL)
    assert indices.dtype == np.uint8
    np.testing.assert_array_equal(indices, np.tile(np.repeat(np.arange(6), 10), (10, 1)))


def test_solid_snaps_gold_to_yellow():
    assert np.all(quantize(Canvas((16, 16), LCARS_MODERN["gold"], Method.SOLID), PANEL) == 2)


def test_pattern_sky_uses_chromatic_mixture():
    indices = quantize(Canvas((64, 64), LCARS_MODERN["sky"], Method.PATTERN), PANEL)
    # Linear-light hue preservation can require red as well as blue/white.
    assert set(np.unique(indices)) <= {1, 3, 4}
    assert 0.2 <= np.mean(indices == 4) <= 0.7


def test_diffuse_preserves_mean():
    indices = quantize(Canvas((64, 64), (128, 128, 128), Method.DIFFUSE), PANEL)
    actual = to_oklab(np.asarray(simulate(indices, PANEL)))[..., 0].mean()
    expected = to_oklab(np.array([128, 128, 128]))[0]
    assert abs(actual - expected) < 0.05


def test_diffuse_neutral_patch_has_low_viewing_distance_chroma():
    canvas = Canvas((128, 128), (150, 152, 148), Method.DIFFUSE)
    viewed = simulate(quantize(canvas, PANEL), PANEL).filter(ImageFilter.GaussianBlur(2))
    chroma = np.linalg.norm(to_oklab(np.asarray(viewed))[..., 1:], axis=-1).mean()
    assert chroma < 0.03


def test_diffuse_python_and_accelerated_paths_agree(monkeypatch):
    if not hasattr(quantizer._diffuse, 'py_func'):
        return  # This installation already runs the Python kernel.
    canvas = Canvas((32, 24), method=Method.DIFFUSE)
    pixels = np.random.default_rng(6).integers(0, 256, (24, 32, 3), dtype=np.uint8)
    canvas.paste(Image.fromarray(pixels), (0, 0), Method.DIFFUSE)
    canvas.region(Method.PATTERN).rectangle((10, 4, 14, 20), fill='gray')
    canvas.region(Method.SOLID).rectangle((20, 4, 24, 20), fill='white')
    accelerated = quantize(canvas, PANEL)
    monkeypatch.setattr(quantizer, '_diffuse', quantizer._diffuse.py_func)
    np.testing.assert_array_equal(quantize(canvas, PANEL), accelerated)


def test_no_error_bleed_into_solid():
    canvas = Canvas((64, 64), (200, 200, 200), Method.DIFFUSE)
    canvas.region(Method.SOLID).rectangle((32, 0, 63, 63), fill=(0, 0, 0))
    assert np.all(quantize(canvas, PANEL)[:, 32:] == 0)


def test_no_error_bleed_into_pattern():
    canvas = Canvas((64, 64), LCARS_MODERN["sky"], Method.PATTERN)
    baseline = quantize(canvas, PANEL)
    canvas.region(Method.DIFFUSE).rectangle((0, 0, 31, 63), fill=(200, 200, 200))
    np.testing.assert_array_equal(quantize(canvas, PANEL)[:, 32:], baseline[:, 32:])


def test_diffuse_serpentine_small_golden():
    # Oklab L(128) ~= .6. First row picks white then black. The next row
    # must start at the right: its error makes the bottom-left pixel black.
    panel = ((0, 0, 0),) + ((255, 255, 255),) * 5
    canvas = Canvas((2, 2), (128, 128, 128), Method.DIFFUSE)
    np.testing.assert_array_equal(quantize(canvas, panel), [[1, 0], [0, 1]])
    np.testing.assert_array_equal(canvas.rgb, np.full((2, 2, 3), 128))


def test_palette_and_oklab():
    assert PANEL_INDEX == dict(black=0, white=1, yellow=2, red=3, blue=4, green=5)
    assert LCARS_MODERN == dict(violet="#ddbbff", sky="#aaaaff", gold="#ffaa00",
                               gray="#666688", orange="#ff7700", light_gray="#9996ba",
                               text_gray="#d3d3d3", red="#dd4444")
    result = to_oklab(np.array([[[0, 0, 0], [255, 255, 255], [255, 0, 0]]]))
    assert result.shape == (1, 3, 3)
    np.testing.assert_allclose(result[0], [[0, 0, 0], [1, 0, 0],
                                        [0.62795536, 0.22486306, 0.12584630]], atol=1e-7)


@pytest.mark.parametrize("name,args,kwargs", [
    ("rectangle", ((2, 2, 16, 16),), {}),
    ("rounded_rectangle", ((2, 2, 16, 16),), {"radius": 4}),
    ("ellipse", ((2, 2, 16, 16),), {}),
    ("polygon", ([(2, 2), (16, 4), (8, 16)],), {}),
    ("line", ([(2, 2), (16, 16)],), {"width": 3}),
    ("pieslice", ((2, 2, 16, 16), 10, 270), {}),
    ("text", ((2, 2), "Hi"), {}),
])
def test_dual_draw(name, args, kwargs):
    canvas = Canvas((24, 24))
    region = canvas.region(Method.SOLID)
    getattr(region, name)(*args, fill="white", **kwargs)
    expected = Image.new("L", canvas.rgb.size, int(Method.PATTERN))
    draw = ImageDraw.Draw(expected)
    draw.fontmode = "1"
    getattr(draw, name)(*args, fill=int(Method.SOLID), **kwargs)
    np.testing.assert_array_equal(canvas.method, expected)
    expected_rgb = Image.new("RGB", canvas.rgb.size)
    getattr(ImageDraw.Draw(expected_rgb), name)(*args, fill="white", **kwargs)
    np.testing.assert_array_equal(canvas.rgb, expected_rgb)


@pytest.mark.parametrize("name,args,kwargs", [
    ("rectangle", ((2, 2, 16, 16),), {}),
    ("rounded_rectangle", ((2, 2, 16, 16),), {"radius": 4}),
    ("ellipse", ((2, 2, 16, 16),), {}),
    ("polygon", ([(2, 2), (16, 4), (8, 16)],), {}),
    ("pieslice", ((2, 2, 16, 16), 10, 270), {}),
])
@pytest.mark.parametrize("colours", [{}, {"fill": None, "outline": None}])
def test_shape_without_fill_or_outline_preserves_mask(name, args, kwargs, colours):
    canvas = Canvas((24, 24))
    before = np.array(canvas.method)
    getattr(canvas.region(Method.SOLID), name)(*args, **kwargs, **colours)
    np.testing.assert_array_equal(canvas.method, before)
    expected_rgb = Image.new("RGB", canvas.rgb.size)
    getattr(ImageDraw.Draw(expected_rgb), name)(*args, **kwargs, **colours)
    np.testing.assert_array_equal(canvas.rgb, expected_rgb)


@pytest.mark.parametrize("name,args", [
    ("line", ([(2, 2), (16, 16)],)),
    ("text", ((2, 2), "Hi")),
])
@pytest.mark.parametrize("colours", [{}, {"fill": None}])
def test_line_and_text_without_fill_mark_mask(name, args, colours):
    canvas = Canvas((24, 24))
    getattr(canvas.region(Method.SOLID), name)(*args, **colours)
    expected = Image.new("L", canvas.rgb.size, int(Method.PATTERN))
    draw = ImageDraw.Draw(expected)
    draw.fontmode = "1"
    getattr(draw, name)(*args, fill=int(Method.SOLID))
    assert np.any(np.asarray(expected) == Method.SOLID)
    np.testing.assert_array_equal(canvas.method, expected)
    expected_rgb = Image.new("RGB", canvas.rgb.size)
    getattr(ImageDraw.Draw(expected_rgb), name)(*args, **colours)
    assert np.any(np.asarray(expected_rgb))
    np.testing.assert_array_equal(canvas.rgb, expected_rgb)


@pytest.mark.parametrize("palette_size", [2, 7])
def test_diffuse_uses_palette_length(palette_size):
    panel = ((0, 0, 0),) * (palette_size - 1) + ((255, 255, 255),)
    canvas = Canvas((2, 2), "white", Method.DIFFUSE)
    np.testing.assert_array_equal(quantize(canvas, panel),
                                  np.full((2, 2), palette_size - 1))


def test_canvas_defaults_and_paste():
    canvas = Canvas()
    assert canvas.rgb.mode == "RGB" and canvas.rgb.size == (1600, 1200)
    assert canvas.method.mode == "L" and canvas.method.getpixel((0, 0)) == Method.PATTERN
    canvas = Canvas((8, 8))
    image = Image.new("RGB", (4, 4), PANEL[3])
    mask = Image.new("L", (4, 4), 0)
    ImageDraw.Draw(mask).rectangle((0, 0, 1, 3), fill=255)
    canvas.paste(image, (2, 2), Method.SOLID, mask)
    assert canvas.rgb.getpixel((2, 2)) == PANEL[3]
    assert canvas.method.getpixel((2, 2)) == Method.SOLID
    assert canvas.rgb.getpixel((4, 2)) == (0, 0, 0)
    assert canvas.method.getpixel((4, 2)) == Method.PATTERN
    canvas.paste(image, (0, 0), Method.DIFFUSE)
    assert canvas.method.getpixel((3, 3)) == Method.DIFFUSE


def test_partial_alpha_paste_preserves_method_labels():
    canvas = Canvas((2, 1), method=Method.SOLID)
    canvas.paste(Image.new("RGB", (2, 1), "white"), (0, 0), Method.DIFFUSE,
                 Image.fromarray(np.array([[0, 128]], dtype=np.uint8)))
    np.testing.assert_array_equal(canvas.method, [[Method.SOLID, Method.DIFFUSE]])
    assert canvas.rgb.getpixel((1, 0)) == (128, 128, 128)


def test_draw_positional_fill_and_text_options():
    canvas = Canvas((32, 32))
    canvas.region(Method.DIFFUSE).rectangle((0, 0, 3, 3), "red")
    assert canvas.rgb.getpixel((1, 1)) == (255, 0, 0)
    assert canvas.method.getpixel((1, 1)) == Method.DIFFUSE
    canvas.region(Method.SOLID).text((4, 4), "Hi", fill="white", font_size=18)
    assert set(np.unique(canvas.method)) <= {0, 1, 2}


def test_simulate_uses_configured_palette():
    indices = np.arange(6, dtype=np.uint8).reshape(2, 3)
    image = simulate(indices, tuple(reversed(PANEL)))
    assert image.mode == "RGB" and image.size == (3, 2)
    np.testing.assert_array_equal(np.asarray(image), np.array(PANEL[::-1]).reshape(2, 3, 3))


CALIBRATED = np.array([[42,39,46], [193,197,179], [214,197,71],
                       [101,41,34], [60,91,145], [86,115,89]])


def _linear(rgb):
    v = np.asarray(rgb) / 255
    return np.where(v <= .04045, v / 12.92, ((v + .055) / 1.055) ** 2.4)


def _inside_hull(points):
    # Independent tetrahedral membership oracle, including hull boundary.
    from itertools import combinations
    vertices = _linear(CALIBRATED)
    inside = np.zeros(points.shape[:-1], dtype=bool)
    for ids in combinations(range(6), 4):
        tetra = vertices[list(ids)]
        matrix = (tetra[1:] - tetra[0]).T
        if abs(np.linalg.det(matrix)) < 1e-12:
            continue
        weights = (points - tetra[0]) @ np.linalg.inv(matrix).T
        inside |= (weights.min(axis=-1) >= -2e-6) & (weights.sum(axis=-1) <= 1+2e-6)
    return inside


def test_gamut_preserves_in_hull():
    weights = np.random.default_rng(19).dirichlet(np.ones(6), 100)
    linear = weights @ _linear(CALIBRATED)
    rgb = 255 * np.where(linear <= .0031308, linear * 12.92,
                         1.055 * linear ** (1/2.4) - .055)
    lab = to_oklab(rgb)
    np.testing.assert_allclose(quantizer._gamut_map(lab, to_oklab(CALIBRATED)), lab,
                               atol=1e-7)


def test_gamut_reachable_and_preserves_hue():
    rgb = np.random.default_rng(20).integers(0, 256, (500, 3))
    lab = to_oklab(rgb)
    mapped = quantizer._gamut_map(lab, to_oklab(CALIBRATED))
    assert _inside_hull(_linear(quantizer._to_rgb(mapped))).all()
    chromatic = np.linalg.norm(mapped[:, 1:], axis=-1) > 1e-5
    cross = lab[:, 1]*mapped[:, 2] - lab[:, 2]*mapped[:, 1]
    assert np.abs(cross[chromatic]).max() < 1e-7
    assert np.all((lab[chromatic, 1:]*mapped[chromatic, 1:]).sum(-1) > 0)


def test_vermilion_has_substantial_red():
    indices = quantize(Canvas((192, 192), (211,120,64), Method.DIFFUSE), CALIBRATED)
    assert np.mean(indices == 3) >= .20
    assert np.mean(indices == 2) <= .70


def test_gamut_preserves_palette_vertices():
    lab = to_oklab(CALIBRATED)
    np.testing.assert_allclose(quantizer._gamut_map(lab, lab), lab, atol=1e-7)


def test_gamut_neutral_endpoints_are_reachable():
    lab = to_oklab(np.array([[0,0,0], [255,255,255], [128,128,128]]))
    mapped = quantizer._gamut_map(lab, to_oklab(CALIBRATED))
    assert _inside_hull(_linear(quantizer._to_rgb(mapped))).all()
    assert np.linalg.norm(mapped[:,1:], axis=-1).max() < 1e-7
    np.testing.assert_allclose(mapped[2], lab[2], atol=1e-7)
    assert mapped[0,0] > lab[0,0]
    assert mapped[1,0] < lab[1,0]


def test_gold_yellow_share_meets_original_mapper():
    indices = quantize(Canvas((192, 192), (205,160,60), Method.DIFFUSE), CALIBRATED)
    # ca28bcd, same 192×192 (205,160,60) patch and calibrated palette.
    # Store the original count rather than rerunning historical code in tests.
    original_yellow_share = 33600 / 36864
    assert np.mean(indices == 2) >= original_yellow_share


def test_nativity_gold_yellow_share_meets_original_mapper():
    from ventanal.art.compose import compose

    with Image.open(Path(__file__).parent / 'fixtures/gamut/nativity.png') as image:
        source = compose(image, (400, 300))
    canvas = Canvas(source.size, method=Method.DIFFUSE)
    canvas.paste(source, (0, 0), Method.DIFFUSE)
    indices = quantize(canvas, CALIBRATED)
    r, g, b = np.asarray(source, dtype=int).transpose(2, 0, 1)
    gold = (r > 150) & (g > 110) & (b < 90) & (r-g < 60)
    # ca28bcd Nativity gold region; distinct from the uniform gold patch.
    assert gold.sum() == 5113
    original_yellow_share = 2612 / 5113
    assert np.mean(indices[gold] == 2) >= original_yellow_share


def test_pattern_orange_calibrated_red_share():
    indices = quantize(Canvas((64, 64), '#ff7700', Method.PATTERN), CALIBRATED)
    assert np.mean(indices == 3) >= .25
    assert len(np.unique(indices)) <= 3


def test_pattern_gold_calibrated_predominantly_yellow():
    indices = quantize(Canvas((64, 64), '#ffaa00', Method.PATTERN), CALIBRATED)
    assert np.mean(indices == 2) > .5
    assert len(np.unique(indices)) <= 3


def test_pattern_position_stability_and_determinism():
    canvas = Canvas((64, 64), '#ff7700', Method.PATTERN)
    baseline = quantize(canvas, CALIBRATED)
    np.testing.assert_array_equal(quantize(canvas, CALIBRATED), baseline)
    np.testing.assert_array_equal(baseline[:8, :8], baseline[8:16, 8:16])
    canvas.region(Method.SOLID).rectangle((9, 9, 25, 25), fill='white')
    changed = quantize(canvas, CALIBRATED)
    mask = np.asarray(canvas.method) == Method.PATTERN
    np.testing.assert_array_equal(changed[mask], baseline[mask])


@pytest.mark.parametrize('name', LCARS_MODERN)
def test_pattern_convex_weights_and_area_shares(name):
    colour = tuple(Image.new('RGB', (1, 1), LCARS_MODERN[name]).getpixel((0, 0)))
    ids, weights = quantizer._pattern_mix(colour, tuple(CALIBRATED.ravel()))
    assert len(ids) <= 3
    assert min(weights) >= 0
    assert sum(weights) == pytest.approx(1)
    indices = quantize(Canvas((8, 8), colour, Method.PATTERN), CALIBRATED)
    for ink, weight in zip(ids, weights):
        assert abs(np.mean(indices == ink) - weight) <= 1/64


def test_pattern_orange_linear_average_preserves_hue():
    indices = quantize(Canvas((64, 64), '#ff7700', Method.PATTERN), CALIBRATED)
    shares = np.bincount(indices.ravel(), minlength=6) / indices.size
    linear = shares @ _linear(CALIBRATED)
    rgb = 255 * np.where(linear <= .0031308, linear * 12.92,
                         1.055 * linear ** (1/2.4) - .055)
    lab = to_oklab(rgb)
    source = to_oklab(np.array([255, 119, 0]))
    difference = np.angle(np.exp(1j * (np.arctan2(lab[2], lab[1]) -
                                      np.arctan2(source[2], source[1]))))
    assert abs(np.degrees(difference)) < 5


def test_pattern_preserves_reachable_three_ink_target():
    # This target lies on a panel triangle and survives selective mapping.
    linear = np.array([.2, .3, .5]) @ _linear(CALIBRATED[[1, 3, 4]])
    rgb = 255 * np.where(linear <= .0031308, linear * 12.92,
                         1.055 * linear ** (1/2.4) - .055)
    ids, weights = quantizer._pattern_mix(tuple(rgb), tuple(CALIBRATED.ravel()))
    np.testing.assert_allclose(np.asarray(weights) @ _linear(CALIBRATED[list(ids)]),
                               linear, atol=1e-7)


def test_pattern_mix_is_cached():
    quantizer._pattern_mix.cache_clear()
    canvas = Canvas((16, 16), '#ff7700', Method.PATTERN)
    quantize(canvas, CALIBRATED)
    before = quantizer._pattern_mix.cache_info()
    quantize(canvas, CALIBRATED)
    after = quantizer._pattern_mix.cache_info()
    assert after.misses == before.misses
    assert after.hits > before.hits


@pytest.mark.parametrize(('method', 'expected'), [
    (Method.SOLID, '13736299887fd67476f25c80485811efa776c8c88fadd2258ca02b09e139bbe6'),
    (Method.DIFFUSE, 'aed525eb1c6c100e7ac1b6b8e94b45243169376701a816262fa3fcd67a11f03b'),
])
def test_non_pattern_matches_pre_orange_fix(method, expected):
    import hashlib
    canvas = Canvas((48, 32), method=method)
    canvas.rgb = Image.fromarray(np.random.default_rng(15).integers(
        0, 256, (32, 48, 3), dtype=np.uint8))
    assert hashlib.sha256(quantize(canvas, CALIBRATED).tobytes()).hexdigest() == expected
