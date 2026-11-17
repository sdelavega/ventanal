# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Stephen de la Vega

"""IEM reflectivity cropped in palette space before classification."""

from dataclasses import dataclass
from datetime import datetime, timedelta, timezone
from io import BytesIO
from math import cos, isfinite, radians

import numpy as np
from PIL import Image, ImageFilter

from ventanal.weather.data import USER_AGENT


IEM = 'https://mesonet.agron.iastate.edu/data/gis/images/4326/USCOMP/n0q_0'


def bbox_around(lat, lon, radius_miles, aspect: float) -> tuple[float, float, float, float]:
    """Use radius as the half-height, with physical width scaled by aspect."""
    if not all(isfinite(v) for v in (lat, lon, radius_miles, aspect)):
        raise ValueError('Coordinates, radius and aspect must be finite')
    if not -90 < lat < 90 or radius_miles <= 0 or aspect <= 0:
        raise ValueError('Latitude, radius or aspect out of range')
    dy = radius_miles / 69.0
    dx = dy * aspect / cos(radians(lat))
    return lon - dx, lat - dy, lon + dx, lat + dy


@dataclass
class Radar:
    levels: np.ndarray
    valid: datetime


def dbz_to_level(dbz: np.ndarray) -> np.ndarray:
    """Classify reflectivity at the inclusive 20, 35 and 50 dBZ boundaries."""
    values = np.asarray(dbz)
    return ((values >= 20).astype(np.uint8) + (values >= 35) + (values >= 50))



def clean_echoes(levels: np.ndarray) -> np.ndarray:
    """Open/close at 3×3, then discard 8-connected echoes below 50 map px.

    Applied at the 782×462 drawing scale, after the >=20 dBZ light-band cut.
    Grayscale morphology preserves nested intensity bands and fills tiny holes.
    Input data stays intact; only surviving regions receive outlines.
    """
    image = Image.fromarray(levels)
    for operation in (ImageFilter.MinFilter, ImageFilter.MaxFilter,
                      ImageFilter.MaxFilter, ImageFilter.MinFilter):
        image = image.filter(operation(3))
    result = np.array(image)
    pending = result > 0
    height, width = pending.shape
    for y, x in zip(*np.nonzero(pending)):
        if not pending[y, x]:
            continue
        region = [(int(y), int(x))]
        pending[y, x] = False
        cursor = 0
        while cursor < len(region):
            cy, cx = region[cursor]
            cursor += 1
            for ny in range(max(0, cy-1), min(height, cy+2)):
                for nx in range(max(0, cx-1), min(width, cx+2)):
                    if pending[ny, nx]:
                        pending[ny, nx] = False
                        region.append((ny, nx))
        if len(region) < 50:
            ys, xs = zip(*region)
            result[ys, xs] = 0
    return result

def _validate_view(bbox, size):
    west, south, east, north = bbox
    width, height = size
    if not all(isfinite(v) for v in bbox) or west >= east or south >= north:
        raise ValueError('Expected finite west < east and south < north')
    if any(not isinstance(v, int) or v <= 0 for v in size):
        raise ValueError('Size must contain positive integer width and height')
    return west, south, east, north, width, height


def _crop_indices(image, world, bbox, size):
    west, south, east, north, width, height = _validate_view(bbox, size)
    a, d, b, e, c, f = (float(v) for v in world.split())
    if not all(isfinite(v) for v in (a, d, b, e, c, f)) or a <= 0 or e >= 0 or b != 0 or d != 0:
        raise ValueError('Expected an unrotated, north-up world file')
    # World files locate pixel centres. Map output centres to nearest source
    # centres; no interpolated palette values and no full-composite ndarray.
    xs = west + (np.arange(width) + 0.5) * ((east - west) / width)
    ys = north - (np.arange(height) + 0.5) * ((north - south) / height)
    cols = np.floor((xs - c) / a + 0.5).astype(np.int64)
    rows = np.floor((ys - f) / e + 0.5).astype(np.int64)
    good_x = (cols >= 0) & (cols < image.width)
    good_y = (rows >= 0) & (rows < image.height)
    output = np.zeros((height, width), dtype=np.uint8)
    if not good_x.any() or not good_y.any():
        return output
    left, right = int(cols[good_x].min()), int(cols[good_x].max()) + 1
    top, bottom = int(rows[good_y].min()), int(rows[good_y].max()) + 1
    # Pillow decodes the 8-bit image once; only the crop is copied into NumPy.
    with image.crop((left, top, right, bottom)) as cropped:
        indices = np.asarray(cropped)
        output[np.ix_(good_y, good_x)] = indices[np.ix_(rows[good_y] - top, cols[good_x] - left)]
    return output


def fetch_iem(bbox, size: tuple[int, int], client, cache_dir) -> Radar | None:
    """Return fresh radar or None on any failure; radar itself is not cached.

    IEM's composite_n0q lookup table reserves index 0 for missing; index 1
    is -32 dBZ, so the documented conversion is idx * 0.5 - 32.5.
    cache_dir is retained for the shared caller interface.
    """
    try:
        def get(suffix):
            response = client.get(IEM + suffix, headers={'User-Agent': USER_AGENT})
            response.raise_for_status()
            return response

        valid = datetime.fromisoformat(get('.json').json()['meta']['valid'])
        if valid.utcoffset() is None:
            return None
        valid = valid.astimezone(timezone.utc)
        if datetime.now(timezone.utc) - valid > timedelta(minutes=30):
            return None
        world = get('.wld').text
        payload = get('.png').content
        with Image.open(BytesIO(payload)) as image:
            if image.mode != 'P':
                return None
            indices = _crop_indices(image, world, bbox, size)
        levels = dbz_to_level(indices.astype(np.float32) * 0.5 - 32.5)
        levels[indices == 0] = 0
        return Radar(levels, valid)
    except Exception:
        # This interface deliberately treats transport, parsing and decoding
        # failures alike, allowing the caller to display radar unavailable.
        return None
