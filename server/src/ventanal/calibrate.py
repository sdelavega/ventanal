# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Stephen de la Vega

"""Six-colour calibration chart and perspective-corrected photo measurements."""

from pathlib import Path
import re
import tomllib

import numpy as np
from PIL import Image

from ventanal.frame import FRAME_H, FRAME_W
from ventanal.palette import PANEL_INDEX


def _patches():
    """Row-major patch rectangles inside the 40 pixel border."""
    xs = np.linspace(40, FRAME_W - 40, 4, dtype=int)
    ys = np.linspace(40, FRAME_H - 40, 3, dtype=int)
    return [(int(xs[x]), int(ys[y]), int(xs[x+1]), int(ys[y+1]))
            for y in range(2) for x in range(3)]


def chart() -> np.ndarray:
    """Return a 3×2 index grid, black border and 40 pixel white corner squares."""
    idx = np.zeros((FRAME_H, FRAME_W), dtype=np.uint8)
    for colour, (left, top, right, bottom) in enumerate(_patches()):
        idx[top:bottom, left:right] = colour
    for y in (0, FRAME_H - 40):
        for x in (0, FRAME_W - 40):
            idx[y:y+40, x:x+40] = PANEL_INDEX['white']
    return idx


def measure(photo: Image.Image | np.ndarray, corners: list[tuple[int, int]]):
    """Warp TL, TR, BR, BL image corners; take each patch's central 60% median.

    Corners identify the outside of the whole chart, including its border.
    Coordinates refer to the pixels in the supplied image (no EXIF rotation).
    Float RGB arrays retain linear precision; PIL inputs retain the CLI convention.
    """
    linear = isinstance(photo, np.ndarray)
    width, height = (photo.shape[1], photo.shape[0]) if linear else photo.size
    points = np.asarray(corners, dtype=float)
    if points.shape != (4, 2) or not np.isfinite(points).all():
        raise ValueError('corners must contain four finite x,y pairs in TL, TR, BR, BL order')
    if np.any(points < 0) or np.any(points[:, 0] >= width) or np.any(points[:, 1] >= height):
        raise ValueError('corners must lie inside the photo')
    edges = np.roll(points, -1, axis=0) - points
    turns = edges[:, 0] * np.roll(edges[:, 1], -1) - edges[:, 1] * np.roll(edges[:, 0], -1)
    if not np.all(turns > 0):
        raise ValueError('corners must form a convex TL, TR, BR, BL quadrilateral')
    rows, values = [], []
    for (x, y), (u, v) in zip([(0, 0), (1599, 0), (1599, 1199), (0, 1199)], points):
        rows.extend([[x, y, 1, 0, 0, 0, -u*x, -u*y],
                     [0, 0, 0, x, y, 1, -v*x, -v*y]])
        values.extend([u, v])
    try:
        coefficients = np.linalg.solve(rows, values)
    except np.linalg.LinAlgError as exc:
        raise ValueError('corners do not define a perspective transform') from exc
    if linear:
        # Floating-point planes preserve RAW precision and linear interpolation.
        warped = np.stack([np.asarray(Image.fromarray(photo[:,:,i]).transform(
            (FRAME_W, FRAME_H), Image.Transform.PERSPECTIVE, coefficients,
            resample=Image.Resampling.BICUBIC)) for i in range(3)],axis=2)
    else:
        warped = np.asarray(photo.convert('RGB').transform(
            (FRAME_W, FRAME_H), Image.Transform.PERSPECTIVE, coefficients,
            resample=Image.Resampling.BICUBIC))
    colours = []
    for left, top, right, bottom in _patches():
        dx, dy = (right-left) * .2, (bottom-top) * .2
        centre = warped[int(np.ceil(top+dy)):int(bottom-dy),
                        int(np.ceil(left+dx)):int(right-dx)]
        median = np.median(centre, axis=(0, 1))
        colours.append(tuple(float(c) if linear else int(round(c)) for c in median))
    return colours


def panel_block(colours: list[tuple[int, int, int]]) -> str:
    """Format measured colours in the configuration's index order."""
    return '[panel]\n' + ''.join(
        f'{key} = "#{r:02x}{g:02x}{b:02x}"\n'
        for key, (r, g, b) in zip(PANEL_INDEX, colours))


# Consume strings and comments as whole tokens, so fake table headers inside
# multiline strings cannot be mistaken for configuration structure.
_STRING = r'"""(?:[^"\\]|\\.|"(?!""))*"""|\'\'\'[\s\S]*?\'\'\'|"(?:[^"\\\r\n]|\\.)*"|\'[^\'\r\n]*\''
_KEY = r'(?:[A-Za-z0-9_-]+|"(?:[^"\\]|\\.)*"|\'[^\']*\')'
_TOKENS = re.compile(
    rf'(?P<table>^[ \t]*\[[^\r\n]*?\][ \t]*(?:\#[^\r\n]*)?(?=\r?$))'
    rf'|^[ \t]*(?P<key>{_KEY})[ \t]*=[ \t]*(?P<value>{_STRING})'
    rf'|(?:{_STRING})|\#[^\r\n]*|(?P<bracket>[\[\]{{}}])', re.MULTILINE)


def write_panel(path: Path, colours: list[tuple[int, int, int]]) -> None:
    """Replace only the six panel string values, preserving every other byte.

    The existing file must contain a [panel] table with all six colour keys.
    Parsing both versions before writing prevents partial or invalid updates.
    """
    from ventanal.settings import backup_config, prune_config_backups, read_document, replace_config, settings_lock

    backup_dir = path.parent / 'backups'
    with settings_lock(path, backup_dir):
        original = read_document(path)
        updated = _replace_panel(original, colours)
        backup_config(backup_dir, original)
        replace_config(path, updated.encode('utf-8'), backup_dir)
        prune_config_backups(backup_dir)


def _replace_panel(original, colours):
    replacements = tomllib.loads(panel_block(colours))['panel']
    spans = []
    in_panel = False
    depth = 0
    for token in _TOKENS.finditer(original):
        if token['bracket'] is not None:
            depth += 1 if token['bracket'] in '[{' else -1
        elif depth:
            continue
        elif token['table'] is not None:
            table = tomllib.loads(token['table'].strip() + '\n__probe__ = 0\n')
            in_panel = table == {'panel': {'__probe__': 0}}
        elif in_panel and token['key'] is not None:
            key = next(iter(tomllib.loads(token['key'] + ' = 0')))
            if key in replacements:
                value = token['value']
                quote = value[:3] if value.startswith(('"""', "'''")) else value[0]
                spans.append((*token.span('value'), quote + replacements[key] + quote))
    if len(spans) != len(PANEL_INDEX):
        raise ValueError('config must contain all six [panel] colour strings')
    updated = original
    for start, end, value in reversed(spans):
        updated = updated[:start] + value + updated[end:]
    if tomllib.loads(updated)['panel'] != {**tomllib.loads(original)['panel'], **replacements}:
        raise ValueError('unable to update panel colours')
    return updated


def srgb_to_linear(rgb, maximum=255):
    """Decode sRGB samples at their original integer scale into linear light."""
    values = np.asarray(rgb, dtype=float) / maximum
    return np.where(values <= .04045, values / 12.92, ((values + .055) / 1.055) ** 2.4)


def white_balance(colours, paper):
    """Scale each linear-light channel so the paper's mean becomes sRGB 243."""
    reference = np.asarray(paper, dtype=float)
    samples = np.asarray(colours, dtype=float)
    if (reference.shape != (3,) or not np.isfinite(reference).all()
            or np.any(reference <= 0) or np.any(reference > 255)
            or samples.ndim != 2 or samples.shape[1] != 3
            or not np.isfinite(samples).all() or np.any(samples < 0) or np.any(samples > 255)):
        raise ValueError('paper and colours must be valid RGB samples; paper channels must be positive')
    balanced = np.clip(srgb_to_linear(samples) * srgb_to_linear(243) / srgb_to_linear(reference), 0, 1)
    encoded = np.where(balanced <= .0031308, 12.92 * balanced, 1.055 * balanced ** (1/2.4) - .055)
    return [tuple(int(c) for c in row) for row in np.rint(encoded * 255)]


def paper_mean(photo, rectangle):
    """Mean sRGB of an in-bounds x,y,width,height white-paper rectangle."""
    if len(rectangle) != 4 or not all(np.isfinite(v) for v in rectangle):
        raise ValueError('paper must be x,y,width,height')
    x, y, w, h = map(int, rectangle)
    if x < 0 or y < 0 or w <= 0 or h <= 0 or x+w > photo.width or y+h > photo.height:
        raise ValueError('paper rectangle must lie inside the photo and have positive size')
    return tuple(np.asarray(photo.convert('RGB').crop((x,y,x+w,y+h))).mean(axis=(0,1)))


def map_corners(points, preview_size, photo_size):
    """Map displayed pixel endpoints to full-resolution pixel endpoints."""
    pw, ph = preview_size
    fw, fh = photo_size
    points = np.asarray(points, dtype=float)
    if (pw < 2 or ph < 2 or points.shape != (4,2) or not np.isfinite(points).all()
            or np.any(points < 0) or np.any(points[:,0] > pw-1) or np.any(points[:,1] > ph-1)):
        raise ValueError('Select four corners inside the preview in TL, TR, BR, BL order.')
    return [tuple(int(v) for v in row) for row in np.rint(points * [(fw-1)/(pw-1),(fh-1)/(ph-1)])]
