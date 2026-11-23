# SPDX-License-Identifier: GPL-3.0-or-later
"""E-paper frames travel zlib-compressed to firmware >= 1.1.0 (same pixels)."""
from datetime import datetime, timedelta, timezone
from http.client import HTTPConnection
import hashlib
import json
from threading import Thread
import zlib

import pytest
from PIL import Image
from ventanal.manifest import Readout, publish, read_manifest, select_epaper_encoding
from ventanal.serve import make_server

NOW = datetime(2026, 11, 23, 12, 0, tzinfo=timezone.utc)


def frame_bytes(seed=0):
    return b'VNF1' + bytes((i + seed) % 256 for i in range(256)) * 3750 + b'\0' * 12


@pytest.fixture
def published(tmp_path):
    data = frame_bytes()
    entry = publish(tmp_path, 'weather', data, Readout(0, 0, 1, 1, 'shade'), None,
                    Image.new('RGB', (1, 1)), NOW)
    srv = make_server(tmp_path, '127.0.0.1', 0)
    thread = Thread(target=srv.serve_forever, daemon=True)
    thread.start()
    yield srv, data, entry
    srv.shutdown(); srv.server_close(); thread.join()


def get(srv, path, headers=None):
    conn = HTTPConnection(*srv.server_address)
    conn.request('GET', path, headers=headers or {})
    response = conn.getresponse()
    result = response.status, dict(response.getheaders()), response.read()
    conn.close()
    return result


def manifest(srv, query):
    status, _, body = get(srv, '/manifest.json' + query)
    assert status == 200
    return json.loads(body)['screens']['weather']


@pytest.mark.parametrize('query', ['?fw=1.1.0', '?fw=1.1.0&board=inkplate13spectra',
                                   '?bat=90&screen=art&fw=1.10.0&board=inkplate13spectra'])
def test_epaper_fw_1_1_gets_zlib_frames(published, query):
    srv, data, original = published
    entry = manifest(srv, query)
    assert entry['encoding'] == 'zlib'
    assert entry['url'] == original['url'][:-len('.vnf')] + '.vnz'
    assert entry['id'] == original['id']
    assert entry['size'] < 960016
    # The hash covers the decoded frame the panel draws, not the wire bytes.
    assert entry['sha256'] == hashlib.sha256(data).hexdigest()
    assert entry['readout'] == original['readout']


@pytest.mark.parametrize('query', ['', '?fw=1.0.0', '?fw=bad', '?fw=1.0.9&board=inkplate13spectra',
                                   '?fw=1.1.0-rc.1', '?fw=1.1.0&board=lcd43b'])
def test_epaper_old_fw_gets_raw_frames(published, query):
    srv, data, original = published
    entry = manifest(srv, query)
    # Exactly today's entry: no encoding, no wire variant, raw .vnf bytes.
    assert entry == {k: v for k, v in original.items() if k not in ('credit', 'zlib')}
    assert get(srv, entry['url'])[2] == data


def test_zlib_frame_inflates_to_identical_vnf(published, tmp_path):
    srv, data, _ = published
    entry = manifest(srv, '?fw=1.1.0')
    status, _, wire = get(srv, entry['url'])
    assert status == 200 and len(wire) == entry['size']
    assert wire[:2] == b'\x78\xda'  # zlib header, level 9
    assert zlib.decompress(wire) == data
    # Produced once at publish time, beside the raw frame it encodes.
    assert (tmp_path / entry['url'].lstrip('/')).read_bytes() == wire


def test_zlib_frame_range_and_etag(published):
    srv, _, _ = published
    entry = manifest(srv, '?fw=1.1.0')
    status, headers, full = get(srv, entry['url'])
    assert status == 200 and headers['ETag'] == f'"{entry["id"]}"'
    assert headers['Accept-Ranges'] == 'bytes'
    status, headers2, part = get(srv, entry['url'], {'Range': 'bytes=2-12', 'If-Range': headers['ETag']})
    assert status == 206 and headers2['ETag'] == headers['ETag'] and part == full[2:13]
    assert headers2['Content-Range'] == f'bytes 2-12/{len(full)}'
    # A stale validator returns the whole current body, never a splice.
    status, _, whole = get(srv, entry['url'], {'Range': 'bytes=2-12', 'If-Range': '"0000000000000000"'})
    assert status == 200 and whole == full


def test_zlib_variants_pruned_with_their_frames(tmp_path):
    for i in range(6):
        publish(tmp_path, 'weather', frame_bytes(i), Readout(0, 0, 1, 1, 'shade'), None,
                Image.new('RGB', (1, 1)), NOW + timedelta(minutes=i))
    frames = tmp_path / 'frames'
    raw = sorted(p.stem for p in frames.glob('weather-*.vnf'))
    wire = sorted(p.stem for p in frames.glob('weather-*.vnz'))
    assert len(raw) == 4 and raw == wire


def test_signed_epaper_zlib_manifest_hashes_decoded_frame(tmp_path):
    from ventanal.security import Security
    data = frame_bytes()
    out = tmp_path / 'out'
    publish(out, 'weather', data, Readout(0, 0, 1, 1, 'shade'), None, Image.new('RGB', (1, 1)), NOW)
    security = Security(tmp_path / 'cache')
    security.enable(['localhost'])
    document = read_manifest(out)
    select_epaper_encoding(document, {'fw': '1.1.0', 'board': 'inkplate13spectra'})
    signed = security.manifest(document, out.resolve())
    entry = signed['screens']['weather']
    assert entry['encoding'] == 'zlib' and entry['url'].endswith('.vnz')
    assert entry['sha256'] == hashlib.sha256(data).hexdigest()
    legacy = read_manifest(out)
    select_epaper_encoding(legacy, {'fw': '1.0.0', 'board': 'inkplate13spectra'})
    assert security.manifest(legacy, out.resolve())['screens']['weather']['sha256'] == entry['sha256']
    # A wire file that no longer matches its published size is never signed.
    wire = out / entry['url'].lstrip('/')
    wire.write_bytes(wire.read_bytes() + b'\0')
    document = read_manifest(out)
    select_epaper_encoding(document, {'fw': '1.1.0', 'board': 'inkplate13spectra'}, out.resolve())
    assert security.manifest(document, out.resolve())['screens']['weather']['url'].endswith('.vnf')


@pytest.mark.parametrize("missing", [True, False])
def test_missing_vnz_falls_back_to_raw_entry(published, tmp_path, missing):
    srv, data, original = published
    wire = tmp_path / original['zlib']['url'].lstrip('/')
    if missing:
        wire.unlink()
    else:
        wire.write_bytes(b'bad')
    entry = manifest(srv, '?fw=1.1.0')
    assert entry['url'] == original['url'] and 'encoding' not in entry
    assert get(srv, entry['url'])[2] == data


def test_missing_vnz_warns_once_per_frame(tmp_path, caplog):
    document = {'screens': {'weather': {'id': 'warning-test-frame', 'url': '/frames/raw.vnf',
                                        'zlib': {'url': '/frames/missing.vnz', 'size': 100}}}}
    import copy
    for _ in range(2):
        selected = copy.deepcopy(document)
        select_epaper_encoding(selected, {'fw': '1.1.0'}, tmp_path.resolve())
        assert selected['screens']['weather']['url'].endswith('.vnf')
    assert sum('warning-test-frame' in record.message for record in caplog.records) == 1
