# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Stephen de la Vega

"""Quantise a colour canvas using a per-pixel choice of dithering method."""

from enum import IntEnum
from functools import lru_cache
from itertools import combinations
from inspect import signature

import numpy as np
from PIL import Image, ImageDraw

from ventanal.palette import to_oklab

try:
    from numba import njit
except ImportError:
    njit = None


class Method(IntEnum):
    DIFFUSE = 0
    PATTERN = 1
    SOLID = 2


class DualDraw:
    """Draw geometry on both the RGB canvas and its discrete method mask."""

    _operations = frozenset({
        "rectangle", "rounded_rectangle", "ellipse", "polygon", "line", "pieslice", "text",
    })

    def __init__(self, canvas: "Canvas", method: Method):
        self._rgb = ImageDraw.Draw(canvas.rgb)
        self._mask = ImageDraw.Draw(canvas.method)
        self._mask.fontmode = "1"
        self._method = int(method)

    def __getattr__(self, name):
        if name not in self._operations:
            raise AttributeError(name)
        rgb_draw = getattr(self._rgb, name)
        mask_draw = getattr(self._mask, name)

        def draw(*args, **kwargs):
            # Bind positional fill/outline arguments too, preserving Pillow's API.
            bound = signature(rgb_draw).bind(*args, **kwargs)
            mask_args = bound.arguments
            for key in ("fill", "outline", "stroke_fill"):
                if key in mask_args and mask_args[key] is not None:
                    mask_args[key] = self._method
            if name in {"line", "text"}:
                mask_args["fill"] = self._method
            elif not any(
                mask_args.get(key) is not None for key in ("fill", "outline")
            ):
                # Shapes with no explicit ink leave the method mask untouched.
                return rgb_draw(*args, **kwargs)
            rgb_draw(*args, **kwargs)
            mask_draw(*bound.args, **bound.kwargs)

        return draw


class Canvas:
    rgb: Image.Image
    method: Image.Image

    def __init__(self, size=(1600, 1200), fill=(0, 0, 0), method=Method.PATTERN):
        self.rgb = Image.new("RGB", size, fill)
        self.method = Image.new("L", size, int(method))

    def region(self, method) -> DualDraw:
        return DualDraw(self, method)

    def paste(self, img: Image.Image, xy, method, mask: Image.Image | None = None):
        self.rgb.paste(img, xy, mask)
        # Alpha may blend RGB, but method labels must stay discrete.
        if mask is not None:
            if mask.mode == "RGBA":
                mask = mask.getchannel("A")
            mask = mask.convert("L").point(lambda value: 255 if value else 0)
        self.method.paste(Image.new("L", img.size, int(method)), xy, mask)


_BAYER = np.array([
    [0, 32, 8, 40, 2, 34, 10, 42],
    [48, 16, 56, 24, 50, 18, 58, 26],
    [12, 44, 4, 36, 14, 46, 6, 38],
    [60, 28, 52, 20, 62, 30, 54, 22],
    [3, 35, 11, 43, 1, 33, 9, 41],
    [51, 19, 59, 27, 49, 17, 57, 25],
    [15, 47, 7, 39, 13, 45, 5, 37],
    [63, 31, 55, 23, 61, 29, 53, 21],
], dtype=np.float64)


def _to_linear(lab):
    """Inverse Oklab without clipping: hull membership needs linear light."""
    lms = lab @ np.array([
        [1, 0.3963377774, 0.2158037573],
        [1, -0.1055613458, -0.0638541728],
        [1, -0.0894841775, -1.2914855480],
    ]).T
    return lms ** 3 @ np.array([
        [4.0767416621, -3.3077115913, 0.2309699292],
        [-1.2684380046, 2.6097574011, -0.3413193965],
        [-0.0041960863, -0.7034186147, 1.7076147010],
    ]).T


def _hull_planes(vertices):
    """Supporting halfspaces for a small panel, without a scipy dependency."""
    planes = []
    for i, j, k in combinations(range(len(vertices)), 3):
        n = np.cross(vertices[j] - vertices[i], vertices[k] - vertices[i])
        length = np.linalg.norm(n)
        if length < 1e-10:
            continue
        n /= length
        d = n @ vertices[i]
        side = vertices @ n - d
        if side.max() <= 1e-8:
            planes.append(np.r_[n, d])
        elif side.min() >= -1e-8:
            planes.append(np.r_[-n, -d])
    return np.asarray(planes).reshape(-1, 4)


@lru_cache(maxsize=16)
def _hue_cusps(palette_key):
    """Numerically maximize reachable Oklab chroma at each integer hue.

    Search the actual linear hull across its entire lightness range, including
    tinted endpoints outside the neutral interval. A coarse two-dimensional
    grid followed by four local refinements resolves L and C to about 1e-5.
    Cached per palette; no frame-sized search or optional optimizer dependency.
    """
    palette = np.asarray(palette_key).reshape(-1, 3)
    planes = _hull_planes(_to_linear(palette))
    angles = np.deg2rad(np.arange(360))
    directions = np.stack((np.cos(angles), np.sin(angles)), axis=-1)
    lightness = np.linspace(palette[:, 0].min(), palette[:, 0].max(), 128)
    chroma = np.linspace(0, .5, 128)
    best_l, best_c = np.zeros(360), np.zeros(360)
    step_l, step_c = lightness[1]-lightness[0], chroma[1]
    for iteration in range(5):
        for start in range(0, 360, 12):
            stop = start + 12
            if iteration == 0:
                ls = np.broadcast_to(lightness, (12, 128))
                cs = np.broadcast_to(chroma, (12, 128))
            else:
                offsets = np.linspace(-1, 1, 17)
                ls = best_l[start:stop, None] + step_l*offsets
                cs = np.maximum(0, best_c[start:stop, None] + step_c*offsets)
            values = np.empty((12, ls.shape[1], cs.shape[1], 3))
            values[..., 0] = ls[:, :, None]
            values[..., 1:] = cs[:, None, :, None]*directions[start:stop, None, None, :]
            linear = _to_linear(values)
            valid = np.ones(values.shape[:-1], dtype=bool)
            for plane in planes:
                valid &= linear @ plane[:3] <= plane[3] + 1e-10
            scores = np.where(valid, values[..., 1]**2 + values[..., 2]**2, -1)
            index = scores.reshape(12, -1).argmax(axis=1)
            chosen = values.reshape(12, -1, 3)[np.arange(12), index]
            best_l[start:stop] = chosen[:, 0]
            best_c[start:stop] = np.linalg.norm(chosen[:, 1:], axis=-1)
        if iteration:
            step_l /= 8
            step_c /= 8
    return best_l


def _gamut_map(lab, palette):
    """Project toward a reachable neutral, retaining Oklab hue exactly.

    The hue cusp supplies the neutral anchor lightness, trading lightness
    for chroma for bright saturated paints.
    Already reachable colours are returned unchanged. Bisection stays on the
    inside of the boundary; tests independently check tetrahedral membership.
    """
    vertices = _to_linear(palette)
    planes = _hull_planes(vertices)
    if len(planes) == 0:
        # Achromatic/duplicate custom palettes have a one-dimensional gamut.
        result = lab.copy()
        result[..., 0] = np.clip(lab[..., 0], palette[:, 0].min(), palette[:, 0].max())
        result[..., 1:] = 0
        return result
    sums = planes[:, :3].sum(axis=1)
    low, high = 0., 1.
    for plane, total in zip(planes, sums):
        if total > 1e-10:
            high = min(high, plane[3] / total)
        elif total < -1e-10:
            low = max(low, plane[3] / total)
    if low > high:
        raise ValueError("Panel gamut must contain a neutral gray")
    cusps = _hue_cusps(tuple(palette.ravel()))
    flat = np.ascontiguousarray(lab.reshape(-1, 3))
    result = flat.copy()
    # Bounded batches avoid full-frame iteration temporaries on the Pi.
    for start in range(0, len(flat), 32768):
        stop = start + 32768
        result[start:stop] = _map_pixels(flat[start:stop], planes,
                                         np.cbrt(low), np.cbrt(high), cusps)
    return result.reshape(lab.shape)


def _map_pixels(lab, planes, low_l, high_l, cusps):
    def inside(values):
        linear = _to_linear(values)
        valid = np.ones(len(values), dtype=bool)
        for plane in planes:
            valid &= linear @ plane[:3] <= plane[3] + 1e-9
        return valid

    result = lab.copy()
    outside = ~inside(lab)
    source = lab[outside]
    if not len(source):
        return result
    hue = np.degrees(np.arctan2(source[:, 2], source[:, 1])) % 360
    origin = np.clip(np.interp(hue, np.arange(361), np.r_[cusps, cusps[0]]),
                     low_l, high_l)
    # Achromatic inputs keep the established neutral endpoint behavior.
    neutral = np.linalg.norm(source[:, 1:], axis=-1) < 1e-7
    origin[neutral] = np.clip(source[neutral, 0], low_l, high_l)
    anchor = np.zeros_like(source)
    anchor[:, 0] = origin
    direction = source - anchor
    lo, hi = np.zeros(len(source)), np.ones(len(source))
    for _ in range(20):
        t = (lo + hi) / 2
        valid = inside(anchor + t[:, None]*direction)
        lo = np.where(valid, t, lo)
        hi = np.where(valid, hi, t)
    result[outside] = anchor + lo[:, None]*direction
    return result


def _diffuse_targets(lab, palette):
    """Production selective mapper for DIFFUSE targets.

    Preserve legacy lightness/chroma for low-error paints: forcing these into
    the physical hull grays gold and skin in the encoded-RGB diffuser. Estimate
    the old hue miss from its nearest ink, ignoring neutral source colours.
    No unconditional 5% chroma compression is applied.
    """
    legacy = lab.copy()
    legacy[..., 0] = np.clip(lab[..., 0], palette[:, 0].min(), palette[:, 0].max())
    chroma = np.linalg.norm(legacy[..., 1:], axis=-1)
    limit = np.linalg.norm(palette[:, 1:], axis=-1).max()
    legacy[..., 1:] *= np.minimum(1, limit/np.maximum(chroma, 1e-12))[..., None]
    # Bound nearest-ink temporary storage on full frames.
    flat = legacy.reshape(-1, 3)
    correct = np.zeros(len(flat), dtype=bool)
    source_chroma = np.linalg.norm(lab[..., 1:], axis=-1).ravel()
    for start in range(0, len(flat), 32768):
        values = flat[start:start+32768]
        nearest = palette[((values[:, None, :]-palette)**2).sum(-1).argmin(-1)]
        angle = np.arctan2(values[:, 2], values[:, 1])
        ink_angle = np.arctan2(nearest[:, 2], nearest[:, 1])
        error = np.abs(np.angle(np.exp(1j*(angle-ink_angle))))
        correct[start:start+len(values)] = (source_chroma[start:start+len(values)] >= .10) & (error > np.deg2rad(30))
    if correct.any():
        flat[correct] = _gamut_map(lab.reshape(-1, 3)[correct], palette)
    return legacy


def _to_rgb(lab):
    """Inverse Oklab, clipped to displayable sRGB for diffusion accounting."""
    lms = lab @ np.array([
        [1, 0.3963377774, 0.2158037573],
        [1, -0.1055613458, -0.0638541728],
        [1, -0.0894841775, -1.2914855480],
    ]).T
    linear = lms ** 3 @ np.array([
        [4.0767416621, -3.3077115913, 0.2309699292],
        [-1.2684380046, 2.6097574011, -0.3413193965],
        [-0.0041960863, -0.7034186147, 1.7076147010],
    ]).T
    srgb = np.where(linear <= 0.0031308, linear * 12.92,
                    1.055 * np.maximum(linear, 0) ** (1 / 2.4) - 0.055)
    return np.clip(srgb * 255, 0, 255)


def _diffuse(rgb, methods, panel_rgb, palette, indices):
    """Serpentine FS: Oklab choices, bounded RGB error, discrete boundaries.

    Account for error in sRGB because the viewing-distance metric averages
    encoded RGB samples. Averaging Oklab samples instead biases their RGB
    mixture toward dark saturated panel colours, especially green.
    Selective targets correct large chromatic hue misses using the physical
    linear-light hull and retain legacy targets for other colours. Accounting
    remains encoded RGB because linear accounting worsens the viewing metric
    (scripts/gamut_metrics.py).
    """
    height, width = methods.shape
    original = rgb.copy()
    span = np.array([panel_rgb[:, ch].max() - panel_rgb[:, ch].min()
                     for ch in range(3)])
    for y in range(height):
        dx = 1 if y % 2 == 0 else -1
        for step in range(width):
            x = step if dx == 1 else width - 1 - step
            if methods[y, x] != 0:
                continue
            colour = rgb[y, x].copy()
            linear = np.empty(3)
            for ch in range(3):
                error = colour[ch] - original[y, x, ch]
                colour[ch] = original[y, x, ch] + min(span[ch], max(-span[ch], error))
                v = max(0.0, colour[ch]) / 255
                linear[ch] = v / 12.92 if v <= 0.04045 else ((v + 0.055) / 1.055) ** 2.4
            r, g, b = linear
            l = (0.4122214708*r + 0.5363325363*g + 0.0514459929*b) ** (1/3)
            m = (0.2119034982*r + 0.6806995451*g + 0.1073969566*b) ** (1/3)
            s = (0.0883024619*r + 0.2817188376*g + 0.6299787005*b) ** (1/3)
            lightness = 0.2104542553*l + 0.7936177850*m - 0.0040720468*s
            a = 1.9779984951*l - 2.4285922050*m + 0.4505937099*s
            b = 0.0259040371*l + 0.7827717662*m - 0.8086757660*s
            best, distance = 0, float("inf")
            for k in range(len(palette)):
                dl = lightness - palette[k, 0]
                da = a - palette[k, 1]
                db = b - palette[k, 2]
                candidate = dl*dl + da*da + db*db
                if candidate < distance:
                    best, distance = k, candidate
            indices[y, x] = best
            for xo, yo, weight in (
                (dx, 0, 7/16), (-dx, 1, 3/16), (0, 1, 5/16), (dx, 1, 1/16),
            ):
                nx, ny = x + xo, y + yo
                if 0 <= nx < width and ny < height and methods[ny, nx] == 0:
                    for ch in range(3):
                        rgb[ny, nx, ch] += (colour[ch] - panel_rgb[best, ch]) * weight
    return indices


if njit is not None:
    _diffuse = njit(_diffuse)


@lru_cache(maxsize=4096)
def _pattern_mix(colour, panel_key):
    """Closest convex simplex of at most three inks in linear light.

    Enumerating faces (including edges/vertices) handles constrained least
    squares without negative weights or an optional optimizer. Interior targets
    can require four inks for exact reconstruction; use their closest triangle.
    """
    panel = np.asarray(panel_key, dtype=np.float64).reshape(-1, 3)
    palette = to_oklab(panel)
    target = _to_linear(_diffuse_targets(to_oklab(np.asarray([colour])), palette))[0]
    # Use the same conversion for targets and ink vertices.
    vertices = _to_linear(palette)
    best_error = float('inf')
    best = None
    for count in range(1, min(3, len(panel)) + 1):
        for ids in combinations(range(len(panel)), count):
            points = vertices[list(ids)]
            if count == 1:
                weights = np.ones(1)
            else:
                basis = (points[1:] - points[0]).T
                rest = np.linalg.lstsq(basis, target - points[0], rcond=None)[0]
                weights = np.r_[1 - rest.sum(), rest]
            if weights.min() < -1e-9:
                continue
            weights = np.maximum(weights, 0)
            weights /= weights.sum()
            error = np.sum((weights @ points - target) ** 2)
            if error < best_error:
                best_error = error
                best = (ids, tuple(weights))
    return best


def quantize(canvas: Canvas, panel_rgb) -> np.ndarray:
    """Return uint8 indices: Oklab snapping/diffusion and linear ink mixtures."""
    rgb = np.array(canvas.rgb, dtype=np.float64)
    methods = np.asarray(canvas.method)
    height, width = methods.shape
    pattern = methods == Method.PATTERN
    lab = to_oklab(rgb)
    palette = to_oklab(np.asarray(panel_rgb))
    indices = np.zeros((height, width), dtype=np.uint8)
    # Bound temporary distance storage to a row rather than H×W×6×3.
    for y in range(height):
        distances = ((lab[y, :, None, :] - palette) ** 2).sum(axis=-1)
        indices[y] = distances.argmin(axis=-1)
    if pattern.any():
        colours, inverse = np.unique(rgb[pattern], axis=0, return_inverse=True)
        thresholds = (_BAYER[np.arange(height)[:, None] % 8,
                              np.arange(width) % 8] + .5) / 64
        # One 8×8 lookup per cached fill mix, anchored to canvas position.
        tiles = []
        panel_key = tuple(np.asarray(panel_rgb, dtype=float).ravel())
        for colour in colours:
            ids, weights = _pattern_mix(tuple(colour), panel_key)
            choices = np.searchsorted(np.cumsum(weights),
                                      (np.arange(64) + .5) / 64)
            tiles.append(np.asarray(ids, dtype=np.uint8)[np.minimum(choices, len(ids)-1)])
        ranks = (thresholds[pattern] * 64).astype(int)
        indices[pattern] = np.asarray(tiles)[inverse, ranks]
    if np.any(methods == Method.DIFFUSE):
        mapped = _to_rgb(_diffuse_targets(lab, palette))
        indices = _diffuse(mapped, methods, np.asarray(panel_rgb, dtype=np.float64),
                           palette, indices)
    return indices


def simulate(indices: np.ndarray, panel_rgb) -> Image.Image:
    """Render panel indices with the configured six reflectance colours."""
    return Image.fromarray(np.asarray(panel_rgb, dtype=np.uint8)[indices])
