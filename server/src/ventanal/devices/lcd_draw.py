# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Stephen de la Vega
"""LCD-only grayscale text coverage and 4x supersampled curved shapes."""
from math import floor, ceil
from PIL import Image, ImageDraw


class LCDDraw:
    """ImageDraw interface with AA curves; straight lines keep crisp edges."""
    def __init__(self, image):
        self.image = image
        self.draw = ImageDraw.Draw(image)
        self.draw.fontmode = 'L'

    def __getattr__(self, name):
        return getattr(self.draw, name)

    def _shape(self, method, xy, fill, **kwargs):
        points = list(zip(xy[::2], xy[1::2])) if isinstance(xy[0], (int, float)) else xy
        left = floor(min(x for x, y in points)) - 3
        top = floor(min(y for x, y in points)) - 3
        right = ceil(max(x for x, y in points)) + 4
        bottom = ceil(max(y for x, y in points)) + 4
        mask = Image.new('L', ((right-left)*4, (bottom-top)*4))
        scaled = [((x-left)*4, (y-top)*4) for x, y in points]
        if method != 'polygon':
            # Inclusive PIL bounds represent whole destination pixels.
            scaled[-1] = (scaled[-1][0]+3, scaled[-1][1]+3)
        getattr(ImageDraw.Draw(mask), method)(scaled, fill=255, **kwargs)
        mask = mask.resize((right-left, bottom-top), Image.Resampling.LANCZOS)
        self.image.paste(fill, (left, top, right, bottom), mask)

    def rounded_rectangle(self, xy, radius=0, fill=None):
        self._shape('rounded_rectangle', xy, fill, radius=radius*4)

    def ellipse(self, xy, fill=None):
        self._shape('ellipse', xy, fill)

    def polygon(self, xy, fill=None):
        self._shape('polygon', xy, fill)


def rounded_mask(size, radius):
    mask = Image.new('L', (size[0]*4, size[1]*4))
    ImageDraw.Draw(mask).rounded_rectangle((0, 0, mask.width-1, mask.height-1),
                                           radius=radius*4, fill=255)
    return mask.resize(size, Image.Resampling.LANCZOS)
