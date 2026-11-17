# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Stephen de la Vega

"""Reproduce Task 6 metrics from server with:
.venv/bin/python scripts/task6_quantize_metrics.py [--write-golden]
The optional accel extra speeds up baseline comparisons.
"""

import argparse
from pathlib import Path

import numpy as np
from PIL import Image, ImageFilter

from ventanal.art.compose import compose
from ventanal.art.render import ART_READOUT, render_art
from ventanal.art.sources import Candidate
from ventanal.config import load_config
from ventanal.palette import to_oklab
from ventanal.quantize import Canvas, Method, quantize, simulate

try:
    from numba import njit
except ImportError:
    njit = None

ROOT = Path(__file__).resolve().parents[2]
CANDIDATE = Candidate('test', '1', 'Hunters in the Snow', 'Pieter Bruegel the Elder',
                      '1565', 'Kunsthistorisches Museum', '', None, None)


def baseline_fs(work, methods, palette, indices):
    """Pre-fix kernel, also applied in scaled RGB for the mockup baseline.

    Scaling RGB by sqrt(.3, .59, .11) reproduces the mockup's weighted squared
    RGB distance, with ordinary RGB Floyd–Steinberg propagation.
    """
    height, width = methods.shape
    for y in range(height):
        dx = 1 if y % 2 == 0 else -1
        for step in range(width):
            x = step if dx == 1 else width - 1 - step
            if methods[y, x] != 0:
                continue
            best, distance = 0, float('inf')
            for k in range(len(palette)):
                candidate = 0.0
                for ch in range(3):
                    candidate += (work[y, x, ch] - palette[k, ch]) ** 2
                if candidate < distance:
                    best, distance = k, candidate
            indices[y, x] = best
            for xo, yo, weight in (
                (dx, 0, 7/16), (-dx, 1, 3/16), (0, 1, 5/16), (dx, 1, 1/16),
            ):
                nx, ny = x + xo, y + yo
                if 0 <= nx < width and ny < height and methods[ny, nx] == 0:
                    for ch in range(3):
                        work[ny, nx, ch] += (work[y, x, ch] - palette[best, ch]) * weight
    return indices


if njit is not None:
    baseline_fs = njit(baseline_fs)


def viewing_error(source, output):
    """Mean Euclidean Oklab distance after identical 2px sRGB Gaussian blur."""
    reference = to_oklab(np.asarray(source.filter(ImageFilter.GaussianBlur(2))))
    viewed = to_oklab(np.asarray(output.filter(ImageFilter.GaussianBlur(2))))
    return float(np.linalg.norm(viewed - reference, axis=-1).mean())


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--write-golden', action='store_true')
    args = parser.parse_args()
    panel = np.asarray(load_config(ROOT / 'server/ventanal.example.toml').panel_rgb)
    with Image.open(ROOT / 'docs/mockups/hunters.jpg') as image:
        backdrop = compose(image)
        art, _ = render_art(image, CANDIDATE)
    painting = Canvas(backdrop.size, method=Method.DIFFUSE)
    painting.paste(backdrop, (0, 0), Method.DIFFUSE)
    print('Mean Oklab distance; 1600x1200; Gaussian sigma=2px; lower is better')
    print('| Kernel | Painting composition | Art screen (pills/text) |')
    print('| --- | ---: | ---: |')
    final = None
    measurements = {}
    for name, space in [('Pre-fix Oklab FS', 'lab'),
                        ('Plain RGB FS (mockup weights)', 'rgb'),
                        ('Plain RGB FS (unweighted)', 'rgb-unweighted'),
                        ('Final bounded perceptual FS', 'final')]:
        scores = []
        for canvas in (painting, art):
            if space == 'final':
                indices = quantize(canvas, panel)
            else:
                rgb = np.asarray(canvas.rgb, dtype=float)
                if space == 'lab':
                    work, palette = to_oklab(rgb), to_oklab(panel)
                else:
                    weights = np.sqrt([0.3, 0.59, 0.11]) if space == 'rgb' else np.ones(3)
                    work, palette = rgb * weights, panel * weights
                methods = np.asarray(canvas.method)
                # Non-DIFFUSE text retains the production SOLID palette choice.
                indices = np.empty(methods.shape, dtype=np.uint8)
                lab = to_oklab(rgb)
                for y in range(methods.shape[0]):
                    indices[y] = ((lab[y, :, None, :] - to_oklab(panel)) ** 2).sum(-1).argmin(-1)
                indices = baseline_fs(work, methods, palette, indices)
            output = simulate(indices, panel)
            scores.append(viewing_error(canvas.rgb, output))
            if canvas is art and space == 'final':
                final = output
        print(f'| {name} | {scores[0]:.8f} | {scores[1]:.8f} |', flush=True)
        measurements[space] = np.array(scores)
    assert np.all(measurements['final'] < measurements['rgb'])
    assert np.all(measurements['final'] < measurements['rgb-unweighted'])
    grey = Canvas((128, 128), (150, 152, 148), Method.DIFFUSE)
    viewed = simulate(quantize(grey, panel), panel).filter(ImageFilter.GaussianBlur(2))
    chroma = np.linalg.norm(to_oklab(np.asarray(viewed))[..., 1:], axis=-1).mean()
    print(f'Neutral patch mean blurred chroma: {chroma:.8f} (< 0.03)')
    assert chroma < 0.03
    r = ART_READOUT
    strip = (slice(r.y + r.h + 60, r.y + r.h + 100), slice(r.x, r.x + r.w))
    original = np.asarray(backdrop)[strip].mean()
    delta = abs(np.asarray(art.rgb)[strip].mean() - original) / original
    print(f'Shadow strip brightness difference: {delta:.4%} (< 8%)')
    assert delta < 0.08
    if args.write_golden:
        final.save(ROOT / 'server/tests/golden/art.png')
        small = ROOT / '.superpowers/sdd/2026-11-15-ventanal/art-golden-v2-small.png'
        final.resize((1000, 750), Image.Resampling.LANCZOS).save(small)
        print('Wrote server/tests/golden/art.png and art-golden-v2-small.png')


if __name__ == '__main__':
    main()
