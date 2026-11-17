# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Stephen de la Vega

"""Packaged typefaces and glyph-safe, width-bounded text."""

from functools import lru_cache
from pathlib import Path
from typing import Literal

from fontTools.ttLib import TTFont
from PIL import ImageFont

FontName = Literal['antonio', 'antonio-bold', 'inter', 'inter-bold']
_FILES = {
    'antonio': 'antonio/Antonio-Regular.ttf',
    'antonio-bold': 'antonio/Antonio-Bold.ttf',
    'inter': 'inter/Inter-Medium.ttf',
    'inter-bold': 'inter/Inter-Bold.ttf',
}


def _path(name: FontName) -> Path:
    return Path(__file__).parent / 'fonts' / _FILES[name]


@lru_cache(maxsize=64)
def font(name: FontName, px: int) -> ImageFont.FreeTypeFont:
    return ImageFont.truetype(str(_path(name)), px)


@lru_cache(maxsize=4)
def _cmap(name: FontName) -> frozenset[int]:
    with TTFont(_path(name)) as face:
        return frozenset(face.getBestCmap())


def renderable(text: str, name: FontName) -> str:
    """Drop unsupported glyphs and normalize whitespace to single spaces."""
    cmap = _cmap(name)
    return ' '.join(''.join(c for c in text if c.isspace() or ord(c) in cmap).split())


def fit(text: str, f: ImageFont.FreeTypeFont, max_w: int) -> str:
    """Fit a prefix plus ellipsis, or return empty if even that cannot fit."""
    if f.getlength(text) <= max_w:
        return text
    if f.getlength('…') > max_w:
        return ''
    lo, hi = 0, len(text)
    while lo < hi:
        mid = (lo + hi + 1) // 2
        if f.getlength(text[:mid].rstrip(' ·') + '…') <= max_w:
            lo = mid
        else:
            hi = mid - 1
    return text[:lo].rstrip(' ·') + '…'
