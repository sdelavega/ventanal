# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Stephen de la Vega

from datetime import datetime, timedelta, timezone
from io import BytesIO
from zipfile import ZipFile

import httpx
import numpy as np
from PIL import Image
import pytest
import shapefile

from ventanal.weather.radar import bbox_around, dbz_to_level, fetch_iem
from ventanal.weather.basemap import basemap

USER_AGENT = 'Ventanal/1.0.0 (+https://github.com/sdelavega/ventanal)'
BBOX = (-0.5, -0.5, 3.5, 3.5)
INDICES = np.array([[0, 104, 105, 134], [135, 164, 165, 255],
                    [105, 135, 165, 0], [104, 134, 164, 165]], dtype=np.uint8)
EXPECTED = np.array([[0, 0, 1, 1], [2, 2, 3, 3],
                     [1, 2, 3, 0], [0, 1, 2, 3]], dtype=np.uint8)


def radar_client(*, age=0, world='1\n0\n0\n-1\n0\n3', png=None, status=200):
    stream = BytesIO()
    image = Image.fromarray(INDICES).convert('P')
    image.save(stream, format='PNG')
    valid = datetime.now(timezone.utc) - timedelta(minutes=age)

    def respond(request):
        assert request.headers['User-Agent'] == USER_AGENT
        if request.url.path.endswith('.json'):
            return httpx.Response(status, json={'meta': {'valid': valid.isoformat()}})
        if request.url.path.endswith('.wld'):
            return httpx.Response(status, text=world)
        assert str(request.url) == 'https://mesonet.agron.iastate.edu/data/gis/images/4326/USCOMP/n0q_0.png'
        return httpx.Response(status, content=stream.getvalue() if png is None else png)
    return httpx.Client(transport=httpx.MockTransport(respond))


def test_bbox_square_miles():
    west, south, east, north = bbox_around(40.01, -105.27, 150, 1)
    assert north - south == pytest.approx(4.34, abs=0.05)
    assert (west + east) / 2 == pytest.approx(-105.27)
    assert (south + north) / 2 == pytest.approx(40.01)
    assert (east - west) * np.cos(np.radians(40.01)) == pytest.approx(north - south)
    wide = bbox_around(40.01, -105.27, 150, 2)
    assert wide[2] - wide[0] == pytest.approx(2 * (east - west))


def test_dbz_levels():
    levels = dbz_to_level(np.array([10, 20, 34.5, 35, 49.5, 50, 70]))
    np.testing.assert_array_equal(levels, [0, 1, 1, 2, 2, 3, 3])
    assert levels.dtype == np.uint8


def test_fetch_failure_none(tmp_path):
    def fail(request):
        raise httpx.ConnectError('offline')
    with httpx.Client(transport=httpx.MockTransport(fail)) as client:
        assert fetch_iem(BBOX, (4, 4), client, tmp_path) is None


def test_index_mapping(tmp_path):
    with radar_client() as client:
        radar = fetch_iem(BBOX, (4, 4), client, tmp_path)
    assert radar is not None
    np.testing.assert_array_equal(radar.levels, EXPECTED)
    assert radar.levels.dtype == np.uint8
    assert radar.valid.utcoffset() == timedelta(0)


def test_crop_nearest_and_outside(tmp_path):
    with radar_client() as client:
        radar = fetch_iem((0.5, 1.5, 2.5, 3.5), (4, 4), client, tmp_path)
        np.testing.assert_array_equal(radar.levels, EXPECTED[:2, 1:3].repeat(2, 0).repeat(2, 1))
        radar = fetch_iem((-1.5, -0.5, 3.5, 3.5), (5, 4), client, tmp_path)
        np.testing.assert_array_equal(radar.levels[:, 0], 0)
        np.testing.assert_array_equal(radar.levels[:, 1:], EXPECTED)
        radar = fetch_iem((100, 100, 110, 110), (3, 2), client, tmp_path)
        np.testing.assert_array_equal(radar.levels, np.zeros((2, 3), dtype=np.uint8))


@pytest.mark.parametrize('kwargs', [
    {'age': 31}, {'world': 'invalid'}, {'world': '1\n1\n0\n-1\n0\n3'},
    {'png': b'broken'}, {'status': 503},
])
def test_invalid_or_stale_none(tmp_path, kwargs):
    with radar_client(**kwargs) as client:
        assert fetch_iem(BBOX, (4, 4), client, tmp_path) is None


def boundary_zip():
    shp, shx, dbf = BytesIO(), BytesIO(), BytesIO()
    with shapefile.Writer(shp=shp, shx=shx, dbf=dbf, shapeType=shapefile.POLYGON) as writer:
        writer.field('name', 'C')
        # Every vertex lies outside the viewport; its horizontal edges cross it.
        writer.poly([[(-2, 0.5), (2, 0.5), (2, -0.5), (-2, -0.5), (-2, 0.5)],
                     [(10, 10), (10, 11), (11, 11), (11, 10), (10, 10)]])
        writer.record('crossing')
    stream = BytesIO()
    with ZipFile(stream, 'w') as archive:
        for suffix, data in [('shp', shp), ('shx', shx), ('dbf', dbf)]:
            archive.writestr(f'boundary.{suffix}', data.getvalue())
    return stream.getvalue()


def test_basemap_clips_and_caches(tmp_path, monkeypatch):
    requests = []
    payload = boundary_zip()

    def respond(request):
        assert request.headers['User-Agent'] == USER_AGENT
        requests.append(str(request.url))
        return httpx.Response(200, content=payload)

    with httpx.Client(transport=httpx.MockTransport(respond)) as client:
        lines = basemap((-1, -1, 1, 1), (101, 101), tmp_path, client)
        assert lines == {kind: [[(0, 25), (100, 25)], [(100, 75), (0, 75)]]
                         for kind in ('states', 'counties')}
        assert requests == [f'https://www2.census.gov/geo/tiger/GENZ2023/shp/cb_2023_us_{kind}_5m.zip'
                            for kind in ('state', 'county')]
        assert len(list(tmp_path.glob('*.zip'))) == 2
        assert basemap((-1, -1, 1, 1), (51, 51), tmp_path, client) != lines
        assert len(requests) == 2
        basemap((-0.5, -1, 1, 1), (101, 101), tmp_path, client)
        assert len(list(tmp_path.glob('basemap-*.json'))) == 3
        def no_reader(*args, **kwargs):
            raise AssertionError('cached polylines must bypass shapefile decoding')
        monkeypatch.setattr(shapefile, 'Reader', no_reader)
        assert basemap((-1, -1, 1, 1), (101, 101), tmp_path, client) == lines
        assert len(requests) == 2
