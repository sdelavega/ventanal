# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Stephen de la Vega

"""LCARS-inspired geometric primitives for a dual RGB/method drawing surface."""

from functools import lru_cache
from math import ceil, cos, pi, sin

from ventanal.quantize import DualDraw


def elbow(d: DualDraw, x, y, w, h, bar, side, r_out, r_in, fill, flip: bool) -> None:
    """Draw the mockup's outer quarter-circle and concave inner quarter-circle."""
    def arc(cx, cy, radius, start, end):
        steps = max(1, ceil(radius * abs(end - start)))
        return [(cx + radius * cos(start + (end - start) * i / steps),
                 cy + radius * sin(start + (end - start) * i / steps))
                for i in range(steps + 1)]

    points = [(x, y + h)]
    points += arc(x + r_out, y + r_out, r_out, pi, 1.5 * pi)
    points += [(x + w, y), (x + w, y + bar)]
    points += arc(x + side + r_in, y + bar + r_in, r_in, -pi / 2, -pi)
    points += [(x + side, y + h)]
    if flip:
        points = [(px, 2 * y + h - py) for px, py in points]
    d.polygon(points, fill=fill)


def pill(d, rect, fill, round_left: bool, round_right: bool) -> None:
    """Draw an (x, y, width, height) pill, optionally squaring either end."""
    x, y, w, h = rect
    if w <= 0 or h <= 0:
        return
    radius = min(w, h) / 2
    d.rounded_rectangle((x, y, x + w - 1, y + h - 1), radius=radius, fill=fill)
    if not round_left:
        d.rectangle((x, y, x + radius, y + h - 1), fill=fill)
    if not round_right:
        d.rectangle((x + w - radius - 1, y, x + w - 1, y + h - 1), fill=fill)


def draw_frame(canvas, segments, accent=None):
    """Shared weather-approved frame; segments are (y, height, palette key).

    Both screens use x=330 for content, a six-pixel sidebar gap, and the
    same readout. Accent defaults to weather violet; labels belong to the caller.
    """
    from ventanal.palette import LCARS_MODERN
    from ventanal.quantize import Method

    pattern = canvas.region(Method.PATTERN)
    m = LCARS_MODERN
    if accent is None:
        accent = m['violet']
    elbow(pattern, 30, 30, 1150, 176, 46, 230, 110, 50, accent, False)
    elbow(pattern, 30, 994, 1540, 176, 46, 230, 110, 50, accent, True)
    pattern.rectangle((1188, 30, 1227, 75), fill=m['orange'])
    pill(pattern, (1236, 30, 334, 46), m['gold'], False, True)
    for y, h, colour in segments:
        pattern.rectangle((30, y, 259, y + h - 1), fill=m[colour])


@lru_cache(maxsize=128)
def _sidebar_ink(colour, panel):
    import numpy as np
    from PIL import ImageColor
    from ventanal.palette import LCARS_MODERN, to_oklab
    from ventanal.quantize import Canvas, quantize, simulate

    tile = Canvas((8, 8), fill=ImageColor.getrgb(LCARS_MODERN[colour]))
    # One complete Bayer tile represents the viewing-distance panel mixture.
    mean = np.asarray(simulate(quantize(tile, panel), panel)).mean(axis=(0, 1))
    background_l = to_oklab(mean)[0]
    ink_l = to_oklab(np.asarray(panel)[:2])[:, 0]
    return panel[int(np.argmax(abs(ink_l - background_l)))]


def sidebar_ink(colour, panel):
    """Black/white ink with greatest Oklab ΔL from the simulated pattern."""
    return _sidebar_ink(colour, tuple(tuple(rgb) for rgb in panel))
