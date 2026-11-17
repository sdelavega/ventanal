# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Stephen de la Vega

"""Panel index order, LCARS-inspired colours, and sRGB to Oklab conversion."""

import numpy as np


PANEL_INDEX = {"black": 0, "white": 1, "yellow": 2, "red": 3, "blue": 4, "green": 5}
LCARS_MODERN = {
    "violet": "#ddbbff", "sky": "#aaaaff", "gold": "#ffaa00",
    "gray": "#666688", "orange": "#ff7700", "light_gray": "#9996ba",
    "text_gray": "#d3d3d3", "red": "#dd4444",
}


def to_oklab(rgb: np.ndarray) -> np.ndarray:
    """Convert (..., 3) sRGB values on the 0–255 scale to float Oklab.

    Extended values are supported for the Bayer offset without clipping.
    """
    srgb = np.asarray(rgb, dtype=np.float64) / 255.0
    linear = np.where(
        srgb <= 0.04045,
        srgb / 12.92,
        ((np.maximum(srgb, 0.0) + 0.055) / 1.055) ** 2.4,
    )
    lms = linear @ np.array([
        [0.4122214708, 0.5363325363, 0.0514459929],
        [0.2119034982, 0.6806995451, 0.1073969566],
        [0.0883024619, 0.2817188376, 0.6299787005],
    ]).T
    return np.cbrt(lms) @ np.array([
        [0.2104542553, 0.7936177850, -0.0040720468],
        [1.9779984951, -2.4285922050, 0.4505937099],
        [0.0259040371, 0.7827717662, -0.8086757660],
    ]).T
