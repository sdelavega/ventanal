# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Stephen de la Vega

"""Census state/county outlines, cached as clipped pixel polylines."""

from hashlib import sha256
from io import BytesIO
import json
import os
from pathlib import Path
from tempfile import NamedTemporaryFile
from zipfile import ZipFile

import shapefile

from ventanal.weather.data import USER_AGENT
from ventanal.weather.radar import _validate_view


def _atomic_write(path, data):
    temporary = None
    try:
        with NamedTemporaryFile(dir=path.parent, delete=False) as stream:
            temporary = Path(stream.name)
            stream.write(data)
        os.replace(temporary, path)
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)


def _clip_segment(start, end, bbox):
    """Liang–Barsky clipping also retains crossings with both ends outside."""
    west, south, east, north = bbox
    x, y = start
    dx, dy = end[0] - x, end[1] - y
    lo, hi = 0.0, 1.0
    for p, q in ((-dx, x - west), (dx, east - x), (-dy, y - south), (dy, north - y)):
        if p == 0:
            if q < 0:
                return None
        elif p < 0:
            lo = max(lo, q / p)
        else:
            hi = min(hi, q / p)
        if lo > hi:
            return None
    return (x + lo * dx, y + lo * dy), (x + hi * dx, y + hi * dy)


def _outlines(reader, bbox, size):
    west, south, east, north = bbox
    width, height = size

    def pixel(point):
        return (max(0, min(width - 1, round((point[0] - west) / (east - west) * (width - 1)))),
                max(0, min(height - 1, round((north - point[1]) / (north - south) * (height - 1)))))

    lines = []
    for shape in reader.iterShapes(bbox=bbox):
        parts = list(shape.parts) + [len(shape.points)]
        for first, last in zip(parts, parts[1:]):
            line = []
            previous_end = None
            for i in range(first, last - 1):
                clipped = _clip_segment(shape.points[i], shape.points[i + 1], bbox)
                if clipped is None:
                    if len(line) > 1:
                        lines.append(line)
                    line, previous_end = [], None
                    continue
                start, end = clipped
                if previous_end != start:
                    if len(line) > 1:
                        lines.append(line)
                    line = [pixel(start)]
                endpoint = pixel(end)
                if endpoint != line[-1]:
                    line.append(endpoint)
                previous_end = end
            if len(line) > 1:
                lines.append(line)
    return lines


def basemap(bbox, size, cache_dir, client) -> dict[str, list[list[tuple[int, int]]]]:
    """Download archives once, then cache outlines by bbox and (width, height).

    Failures propagate to the caller. No network requests or shape decoding
    occur on a polyline cache hit, and archives are never extracted to disk.
    """
    _validate_view(bbox, size)
    bbox = tuple(float(v) for v in bbox)
    size = tuple(size)
    root = Path(cache_dir)
    root.mkdir(parents=True, exist_ok=True)
    key = sha256(json.dumps(['census-2023-5m-v2', bbox, size]).encode()).hexdigest()
    path = root / f'basemap-{key}.json'
    try:
        cached = json.loads(path.read_text())
        return {kind: [[tuple(point) for point in line] for line in cached[kind]]
                for kind in ('states', 'counties')}
    except (OSError, ValueError, TypeError, KeyError):
        pass
    lines = {}
    for kind, layer in (('state', 'states'), ('county', 'counties')):
        name = f'cb_2023_us_{kind}_5m.zip'
        archive_path = root / name
        if not archive_path.exists():
            response = client.get('https://www2.census.gov/geo/tiger/GENZ2023/shp/' + name,
                                  headers={'User-Agent': USER_AGENT})
            response.raise_for_status()
            # Reject invalid downloads before publishing the cache entry.
            with ZipFile(BytesIO(response.content)) as archive:
                if not any(member.endswith('.shp') for member in archive.namelist()):
                    raise ValueError('Boundary archive has no shapefile')
            _atomic_write(archive_path, response.content)
        with ZipFile(archive_path) as archive:
            member = next(name for name in archive.namelist() if name.endswith('.shp'))
            # Only geometry is needed; skip DBF and SHX loading. iterShapes
            # filters shape bboxes before decoding their points.
            with archive.open(member) as stream, shapefile.Reader(shp=stream) as reader:
                lines[layer] = _outlines(reader, bbox, size)
    _atomic_write(path, json.dumps(lines, separators=(',', ':')).encode())
    return lines
