# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Stephen de la Vega
"""Chroma and hue diversity on decoded art, before credits or dithering."""

import numpy as np
from PIL import Image

from ventanal.palette import to_oklab


def colour_metrics(image: Image.Image) -> tuple[float, float]:
    """Return chromatic pixel share and chroma weight outside a 60° hue band.

    Use the most populated circular band, including wraparound. Weight by
    chroma so neutral paper/noise cannot manufacture hue diversity. Compose
    transparency onto white and sample at most 256×256 pixels.
    """
    rgba = image.convert('RGBA')
    rgba.thumbnail((256, 256), Image.Resampling.LANCZOS)
    rgb = Image.new('RGBA', rgba.size, 'white')
    rgb.alpha_composite(rgba)
    lab = to_oklab(np.asarray(rgb.convert('RGB')))
    chroma = np.hypot(lab[..., 1], lab[..., 2])
    mask = chroma > 0.02
    fraction = float(np.mean(mask))
    if not np.any(mask):
        return fraction, 0.0
    angles = np.arctan2(lab[..., 2][mask], lab[..., 1][mask])
    histogram, _ = np.histogram(
        angles, bins=72, range=(-np.pi, np.pi), weights=chroma[mask]
    )
    circular = np.concatenate((histogram, histogram[:11]))
    dominant = np.convolve(circular, np.ones(12), mode='valid').max()
    outside = max(0.0, 1.0 - float(dominant / histogram.sum()))
    return fraction, outside


def colour_fraction(image: Image.Image) -> float:
    """Admission score: chromatic share, zero for a single dominant hue.

    Require 8% chromatic pixels and at least 3% chroma weight outside the
    dominant 60° band. Metadata-confirmed paintings/illuminations bypass this
    heuristic in ArtPool; muted paintings retain their intended eligibility.
    """
    fraction, outside = colour_metrics(image)
    return fraction if outside >= 0.03 else 0.0


def is_colourful(image: Image.Image) -> bool:
    return colour_fraction(image) >= 0.08
