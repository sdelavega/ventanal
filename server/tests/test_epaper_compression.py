# SPDX-License-Identifier: GPL-3.0-or-later
"""E-paper frames travel zlib-compressed to firmware >= 1.1.0 (same pixels)."""
from datetime import datetime, timedelta, timezone
from http.client import HTTPConnection
from pathlib import Path
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


def test_symlinked_output_root_keeps_zlib(tmp_path):
    out = tmp_path / 'actual'
    publish(out, 'weather', frame_bytes(), Readout(0, 0, 1, 1, 'shade'), None, Image.new('RGB', (1, 1)), NOW)
    link = tmp_path / 'link'
    link.symlink_to(out, target_is_directory=True)
    document = read_manifest(link)
    select_epaper_encoding(document, {'fw': '1.1.0'}, link)
    assert document['screens']['weather']['encoding'] == 'zlib'


def test_missing_warnings_are_bounded_and_thread_safe(tmp_path, caplog):
    import copy
    from concurrent.futures import ThreadPoolExecutor
    from ventanal import manifest as module
    document = {'screens': {'weather': {'id': 'concurrent-warning', 'url': '/frames/raw.vnf',
                'zlib': {'url': '/frames/concurrent-missing.vnz', 'size': 100}}}}
    def select(_):
        select_epaper_encoding(copy.deepcopy(document), {'fw': '1.1.0'}, tmp_path)
    with ThreadPoolExecutor(max_workers=16) as pool:
        list(pool.map(select, range(100)))
    assert sum('concurrent-warning' in r.message for r in caplog.records) == 1
    for i in range(1100):
        document['screens']['weather']['id'] = str(i)
        document['screens']['weather']['zlib']['url'] = f'/frames/missing-{i}.vnz'
        select(0)
    assert len(module._missing_epaper_warnings) <= 1024


def test_zlib_symlink_outside_output_root_falls_back(tmp_path):
    root = tmp_path / 'out'
    root.mkdir()
    external = tmp_path / 'external.vnz'
    external.write_bytes(b'wire')
    (root / 'wire.vnz').symlink_to(external)
    document = {'screens': {'weather': {'id': 'escape-test', 'url': '/raw.vnf',
                'zlib': {'url': '/wire.vnz', 'size': 4}}}}
    select_epaper_encoding(document, {'fw': '1.1.0'}, root)
    assert document['screens']['weather']['url'] == '/raw.vnf'


@pytest.mark.parametrize("kind", ["root", "contained", "escape"])
def test_signed_wire_symlink_selection_over_http(tmp_path, monkeypatch, kind):
    import ssl
    import hmac
    from ventanal.security import canonical
    from http.client import HTTPSConnection
    from types import SimpleNamespace
    from test_security import proof
    monkeypatch.setattr('socket.getfqdn', lambda *args: 'localhost')
    out = tmp_path / 'out'
    data = frame_bytes()
    original = publish(out, 'weather', data, Readout(0, 0, 1, 1, 'shade'), None, Image.new('RGB', (1, 1)), NOW)
    wire = out / original['zlib']['url'].lstrip('/')
    if kind != 'root':
        target = (out / 'frames' / 'other.vnz') if kind == 'contained' else (tmp_path / 'other.vnz')
        wire.rename(target)
        wire.symlink_to(target)
    link = tmp_path / 'link'
    link.symlink_to(out, target_is_directory=True)
    cache = tmp_path / 'cache'
    config = tmp_path / 'settings.toml'
    example = Path(__file__).parents[1] / 'ventanal.example.toml'
    config.write_text(example.read_text().replace('/var/lib/ventanal', str(tmp_path)).replace('/var/cache/ventanal', str(cache)))
    monkeypatch.setattr('ventanal.solar.licence.verify', lambda *a: SimpleNamespace(ok=True))
    srv = make_server(link, '127.0.0.1', 0, cache_dir=cache, config_path=config, https_host='127.0.0.1', https_port=0)
    thread = Thread(target=srv.serve_forever, daemon=True); thread.start()
    try:
        srv.ui.enable_security()
        srv.ui.auth.set_password('test password', 'test password')
        ctx = ssl.create_default_context(cadata=srv.ui.security.certificate)
        conn = HTTPSConnection(*srv.tls_server.server_address, context=ctx, timeout=5)
        path = '/manifest.json?fw=1.1.0'
        conn.request('GET', path, headers=proof(srv.ui.security.key, path, 1))
        response = conn.getresponse(); body = response.read(); conn.close()
        assert response.status == 200
        document = json.loads(body)
        signature = document.pop('sig')
        assert hmac.compare_digest(signature, hmac.new(srv.ui.security.key, canonical(document), hashlib.sha256).hexdigest())
        entry = document['screens']['weather']
        assert entry['url'] == (original['zlib']['url'] if kind == 'root' else original['url'])
        conn = HTTPSConnection(*srv.tls_server.server_address, context=ctx, timeout=5)
        conn.request('GET', entry['url'], headers=proof(srv.ui.security.key, entry['url'], 2))
        asset = conn.getresponse(); assert asset.status == 200; asset.read(); conn.close()
    finally:
        srv.shutdown(); srv.server_close(); thread.join()
