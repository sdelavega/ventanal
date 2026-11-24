# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Stephen de la Vega

from http.client import HTTPConnection, HTTPMessage
from threading import Thread
import pytest
from ventanal.serve import make_server


@pytest.fixture
def server(tmp_path):
    (tmp_path / 'manifest.json').write_text('{}')
    (tmp_path / 'frames').mkdir()
    (tmp_path / 'frames' / 'art.vnf').write_bytes(b'frame')
    (tmp_path / 'previews').mkdir()
    (tmp_path / 'previews' / 'art.png').write_bytes(b'png')
    (tmp_path / 'secret').write_text('private')
    (tmp_path / 'frames' / 'escape.vnf').symlink_to(tmp_path / 'secret')
    srv = make_server(tmp_path, '127.0.0.1', 0)
    thread = Thread(target=srv.serve_forever, daemon=True)
    thread.start()
    yield srv
    srv.shutdown()
    srv.server_close()
    thread.join()


@pytest.mark.parametrize(('path', 'status'), [('/../etc/passwd', 404), ('/frames/../../x', 404), ('/frames/%2e%2e/secret.vnf', 404), ('/frames/escape.vnf', 404), ('/manifest.json', 200), ('/frames/art.vnf', 200), ('/previews/art.png', 200), ('/secret', 404), ('/frames/', 404)])
def test_serve_traversal(server, path, status):
    conn = HTTPConnection(*server.server_address)
    conn.request('GET', path)
    response = conn.getresponse()
    assert response.status == status
    response.read()
    conn.close()


def test_get_only(server):
    conn = HTTPConnection(*server.server_address)
    conn.request('HEAD', '/manifest.json')
    assert conn.getresponse().status != 200
    conn.close()


def test_oversized_manifest_returns_503_with_empty_body(server, monkeypatch, tmp_path, caplog):
    (tmp_path / 'manifest.json').write_text('{"version":1,"screens":{}}')
    monkeypatch.setattr('ventanal.serve.MAX_SERVED_MANIFEST_BYTES', 1)
    conn = HTTPConnection(*server.server_address, timeout=2)
    try:
        conn.request('GET', '/manifest.json')
        response = conn.getresponse()
        assert response.status == 503
        assert response.getheader('Content-Length') == '0'
        # The HTTP/1.0 request closes the connection, so inspect even bytes
        # accidentally written beyond the advertised zero-length body.
        assert response.fp.read() == b''
        assert 'Oversize manifest (' in caplog.text and 'returning 503' in caplog.text
    finally:
        conn.close()


@pytest.mark.parametrize('error', [BrokenPipeError, ConnectionResetError])
def test_disconnected_body_copy_is_quiet(server, monkeypatch, error):
    from io import BytesIO
    handler = object.__new__(server.RequestHandlerClass)
    handler.path = '/frames/art.vnf'
    handler.headers = HTTPMessage()
    handler.wfile = BytesIO()
    monkeypatch.setattr(handler, 'send_response', lambda *a: None)
    monkeypatch.setattr(handler, 'send_header', lambda *a: None)
    monkeypatch.setattr(handler, 'end_headers', lambda: None)
    class Disconnected:
        def write(self, data):
            raise error('disconnected')
    handler.wfile = Disconnected()
    handler.do_GET()


def test_tls_idle_timeout_is_30s():
    from ventanal.serve import TLS_IDLE_TIMEOUT
    assert TLS_IDLE_TIMEOUT == 30
