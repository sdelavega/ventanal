# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Stephen de la Vega

from concurrent.futures import ThreadPoolExecutor
from contextlib import closing
import hashlib
from http.client import HTTPConnection, HTTPSConnection
import json
import ssl
from threading import Thread

import pytest

# Importing allow_test_release_key registers its autouse fixture in this module.
from test_ota import BOARD, IMAGE, IMAGE_PATH, allow_test_release_key, signed, web
from test_security import proof
from ventanal.ota import verified_metadata
from ventanal.serve import make_server


FRAME = bytes(range(256)) * 300
FRAME_ID = hashlib.sha256(FRAME).hexdigest()[:16]
FRAME_PATH = f'/frames/extra-test-{FRAME_ID}.vnf'
LCD_PATH = f'/devices/hall/frames/art-{FRAME_ID}.vnr'


def get(server, path, headers=None, *, tls=False):
    if tls:
        context = ssl.create_default_context(cadata=server.ui.security.certificate)
        connection = HTTPSConnection(*server.tls_server.server_address, context=context, timeout=5)
    else:
        connection = HTTPConnection(*server.server_address, timeout=5)
    try:
        connection.request('GET', path, headers=headers or {})
        response = connection.getresponse()
        return response, response.read()
    finally:
        connection.close()


@pytest.fixture
def server(tmp_path):
    for path in (FRAME_PATH, LCD_PATH):
        target = tmp_path / path.lstrip('/')
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(FRAME)
        manifest = {'screens': {'art': {'id': FRAME_ID, 'url': path, 'size': len(FRAME)}}}
        (target.parent.parent / 'manifest.json').write_text(json.dumps(manifest))
    (tmp_path / 'frames' / 'escape.vnf').symlink_to(tmp_path / FRAME_PATH.lstrip('/'))
    (tmp_path / 'previews').mkdir()
    (tmp_path / 'previews' / 'art.png').write_bytes(b'png')
    server = make_server(tmp_path, '127.0.0.1', 0)
    thread = Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        yield server
    finally:
        server.shutdown()
        server.server_close()
        thread.join()


@pytest.mark.parametrize('path', [FRAME_PATH, LCD_PATH])
def test_range_206(server, path):
    response, body = get(server, path, {'Range': 'bytes=10-70000'})
    assert response.status == 206
    assert response.getheader('Accept-Ranges') == 'bytes'
    assert response.getheader('Content-Range') == f'bytes 10-70000/{len(FRAME)}'
    assert response.getheader('Content-Length') == '69991'
    assert response.getheader('ETag') == f'"{FRAME_ID}"'
    assert body == FRAME[10:70001]


@pytest.mark.parametrize(('range_value', 'start'), [
    ('bytes=76796-', 76796), ('bytes=-4', 76796),
    ('bytes=76796-90000', 76796), ('bytes=-90000', 0),
    ('bytes=76796-' + '9' * 5000, 76796),
])
def test_open_ended_and_suffix(server, range_value, start):
    response, body = get(server, FRAME_PATH, {'Range': range_value})
    assert response.status == 206
    assert response.getheader('Content-Range') == f'bytes {start}-{len(FRAME) - 1}/{len(FRAME)}'
    assert body == FRAME[start:]


@pytest.mark.parametrize('range_value', ['bytes=76800-', 'bytes=-0', 'bytes=' + '9' * 5000 + '-'])
def test_unsatisfiable_416(server, range_value):
    response, body = get(server, FRAME_PATH, {'Range': range_value})
    assert response.status == 416
    assert response.getheader('Content-Range') == f'bytes */{len(FRAME)}'
    assert response.getheader('Content-Length') == '0'
    assert body == b''


@pytest.mark.parametrize('validator', ['"different"', f'W/"{FRAME_ID}"', FRAME_ID])
def test_if_range_mismatch_full(server, validator):
    response, body = get(server, FRAME_PATH, {'Range': 'bytes=1-2', 'If-Range': validator})
    assert response.status == 200
    assert response.getheader('Content-Range') is None
    assert body == FRAME


def test_if_range_exact_match(server):
    response, body = get(server, FRAME_PATH, {'Range': 'bytes=1-2', 'If-Range': f'"{FRAME_ID}"'})
    assert response.status == 206
    assert body == FRAME[1:3]


def test_multi_range_full(server):
    response, body = get(server, FRAME_PATH, {'Range': 'bytes=0-1,4-5'})
    assert response.status == 200
    assert response.getheader('Content-Range') is None
    assert body == FRAME


@pytest.mark.parametrize('path', [FRAME_PATH, LCD_PATH])
def test_etag_is_frame_id(server, path):
    response, body = get(server, path)
    assert response.status == 200
    assert response.getheader('ETag') == f'"{FRAME_ID}"'
    assert response.getheader('Accept-Ranges') == 'bytes'
    assert body == FRAME


@pytest.mark.parametrize(('path', 'status', 'body'), [
    ('/frames/escape.vnf', 404, None), ('/frames/%2e%2e/secret.vnf', 404, None),
    ('/previews/art.png', 200, b'png'),
])
def test_range_preserves_route_rules(server, path, status, body):
    response, actual = get(server, path, {'Range': 'bytes=0-0'})
    assert response.status == status
    assert response.getheader('Accept-Ranges') is None
    assert response.getheader('ETag') is None
    if body is not None:
        assert actual == body


def test_auth_still_required(web):
    server, _, root = web
    frame = root / FRAME_PATH.lstrip('/')
    frame.parent.mkdir()
    frame.write_bytes(FRAME)
    server.ui.enable_security()
    server.ui.auth.set_password('test password', 'test password')

    no_range, rejection = get(server, FRAME_PATH, tls=True)
    ranged, body = get(server, FRAME_PATH, {'Range': 'bytes=0-3'}, tls=True)
    assert no_range.status == ranged.status == 403
    assert ranged.getheader('ETag') is None
    assert body == rejection

    headers = {**proof(server.ui.security.key, FRAME_PATH, 1), 'Range': 'bytes=0-3'}
    response, body = get(server, FRAME_PATH, headers, tls=True)
    assert response.status == 206
    assert response.getheader('Cache-Control') == 'no-store'
    assert body == FRAME[:4]


def test_firmware_range_uses_release_sha256(web, signed):
    server, _, _ = web
    server.ui.auth.none()
    release = verified_metadata(*signed())
    server.ui.firmware.cache(release, IMAGE)
    etag = '"' + json.loads(release.metadata)['sha256'] + '"'

    full, body = get(server, IMAGE_PATH)
    assert full.status == 200
    assert full.getheader('ETag') == etag
    assert full.getheader('Accept-Ranges') == 'bytes'
    assert body == IMAGE

    partial, body = get(server, IMAGE_PATH, {'Range': 'bytes=1-3', 'If-Range': etag})
    assert partial.status == 206
    assert partial.getheader('ETag') == etag
    assert partial.getheader('Cache-Control') == 'no-store'
    assert partial.getheader('Content-Range') == f'bytes 1-3/{len(IMAGE)}'
    assert partial.getheader('Content-Length') == '3'
    assert body == IMAGE[1:4]


def test_firmware_range_rejects_corrupt_image(web, signed):
    server, _, _ = web
    server.ui.auth.none()
    store = server.ui.firmware
    store.cache(verified_metadata(*signed()), IMAGE)
    with closing(store.connect()) as db, db:
        db.execute('UPDATE firmware_releases SET image=?', (bytes(len(IMAGE)),))

    response, _ = get(server, IMAGE_PATH, {'Range': 'bytes=1-3'})
    assert response.status == 404
    assert response.getheader('ETag') is None


def test_firmware_materialized_once(web, signed, monkeypatch):
    server, _, _ = web
    server.ui.auth.none()
    store = server.ui.firmware
    store.cache(verified_metadata(*signed()), IMAGE)
    materialize = store._materialize_image
    calls = []

    def counted_materialize(*args):
        calls.append(1)
        return materialize(*args)

    monkeypatch.setattr(store, '_materialize_image', counted_materialize)

    def request_byte(offset):
        response, body = get(server, IMAGE_PATH, {'Range': f'bytes={offset}-{offset}'})
        return response.status, body

    with ThreadPoolExecutor(max_workers=4) as requests:
        assert list(requests.map(request_byte, range(len(IMAGE)))) == [
            (206, bytes([value])) for value in IMAGE
        ]
    response, body = get(server, IMAGE_PATH, {'Range': f'bytes={len(IMAGE)}-'})
    assert (response.status, body) == (416, b'')
    response, body = get(server, IMAGE_PATH, {'Range': 'bytes=0-0', 'If-Range': '"old"'})
    assert (response.status, body) == (200, IMAGE)
    assert len(calls) == 1


@pytest.mark.parametrize('damage', ['missing', 'wrong size'])
def test_firmware_cache_recreated_when_missing(web, signed, damage):
    server, _, root = web
    server.ui.auth.none()
    release = verified_metadata(*signed())
    server.ui.firmware.cache(release, IMAGE)
    path = root / 'cache' / 'firmware' / f'{release.info.sha256}.bin'
    assert not path.exists()
    assert get(server, IMAGE_PATH)[1] == path.read_bytes() == IMAGE
    if damage == 'missing':
        path.unlink()
    else:
        path.write_bytes(IMAGE[:1])

    response, body = get(server, IMAGE_PATH, {'Range': 'bytes=1-3'})
    assert (response.status, body) == (206, IMAGE[1:4])
    assert path.read_bytes() == IMAGE


def test_firmware_cache_pruned_for_removed_release(web, signed):
    server, _, root = web
    server.ui.auth.none()
    store = server.ui.firmware
    release = verified_metadata(*signed())
    store.cache(release, IMAGE)
    assert get(server, IMAGE_PATH)[1] == IMAGE
    path = root / 'cache' / 'firmware' / f'{release.info.sha256}.bin'
    assert path.exists()

    store.cache(verified_metadata(*signed(version='3.0.0')), IMAGE)
    new_image = b'new firmware'
    for version in ('4.0.0', '5.0.0'):
        store.cache(verified_metadata(*signed(version=version, image=new_image)), new_image)
    assert get(server, IMAGE_PATH)[0].status == 404
    assert path.exists()
    assert get(server, f'/firmware/{BOARD}-3.0.0.bin')[1] == IMAGE

    store.cache(verified_metadata(*signed(version='6.0.0', image=new_image)), new_image)
    response, body = get(server, f'/firmware/{BOARD}-6.0.0.bin', {'Range': 'bytes=0-2'})
    assert (response.status, body) == (206, new_image[:3])
    assert not path.exists()
