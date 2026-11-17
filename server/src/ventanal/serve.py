# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Stephen de la Vega

"""A read-only HTTP surface limited to published display assets."""

from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import logging
import json
import ssl
import socket
from threading import Thread, RLock
from pathlib import Path
import re
from urllib.parse import unquote, urlsplit, parse_qs
from datetime import datetime, timezone
import fcntl
from ventanal.owner import OwnerStore, parse_skips


def make_server(out_dir: Path, host: str, port: int, *, cache_dir: Path | None = None, config_path: Path | None = None, rerender=None, https_host=None, https_port=None) -> ThreadingHTTPServer:
    root = out_dir.resolve()

    from ventanal.status import BoardStore, parse_board
    if config_path is not None:
        from ventanal.ui import UI
        ui = UI(config_path, cache_dir, rerender)
    else:
        ui = None

    def active():
        return ui is not None and ui.auth.mode == 'password' and ui.security.ready

    class Handler(BaseHTTPRequestHandler):
        @property
        def protocol_version(self):
            return 'HTTP/1.1' if active() and isinstance(self.connection, ssl.SSLSocket) else 'HTTP/1.0'

        def transport(self):
            if not active(): return True
            path = unquote(urlsplit(self.path).path)
            board = path == '/manifest.json' or path.startswith('/frames/')
            if not isinstance(self.connection, ssl.SSLSocket):
                if board or path.startswith('/previews/'):
                    self.send_error(403)
                else:
                    # Never use an untrusted Host header to construct redirects.
                    hostname = socket.gethostname() if host in ('0.0.0.0', '::') else host
                    authority = f'[{hostname}]' if ':' in hostname else hostname
                    ui.respond(self, 308, location=f'https://{authority}:{server.tls_server.server_address[1]}'+self.path)
                return False
            if board and (len(self.headers.get_all('Authorization', [])) != 1 or
                          len(self.headers.get_all('X-Ventanal-Counter', [])) != 1 or
                          not ui.security.accept(self.command, self.path, self.headers)):
                logging.getLogger(__name__).info('Board proof rejected')
                self.send_error(403); return False
            if board:
                logging.getLogger(__name__).info('Board proof accepted counter=%s', self.headers['X-Ventanal-Counter'])
            return True

        def do_GET(self):
            if not self.transport(): return
            path = unquote(urlsplit(self.path).path)
            if ui is not None and not re.fullmatch(r'/(?:manifest\.json|frames/.*|previews/.*)', path):
                ui.handle(self, path)
                return
            if not re.fullmatch(r'/(?:manifest\.json|frames/[A-Za-z0-9_-]+\.vnf|previews/[A-Za-z0-9_-]+\.png)', path):
                self.send_error(404)
                return
            if path == '/manifest.json' and cache_dir is not None:
                query = parse_qs(urlsplit(self.path).query, keep_blank_values=True)
                board = parse_board(query)
                BoardStore(cache_dir).checkin(self.client_address[0], board)
                values = query.get('skips', [])
                if values:
                    store = OwnerStore(cache_dir)
                    with (cache_dir / 'tick.lock').open('a') as lock:
                        fcntl.flock(lock, fcntl.LOCK_EX)
                        for skipped, shown in parse_skips(','.join(values)):
                            store.record(skipped, shown, datetime.now(timezone.utc))
            if ui is not None and path == '/manifest.json':
                try:
                    with (root / 'manifest.json').open() as f:
                        manifest = json.load(f)
                    from ventanal.config import load_config
                    cfg = load_config(config_path)
                    from ventanal.solar.licence import verify
                    if not verify(cfg.cache_dir/'fcc.sqlite3', cfg.ham_call_sign, cfg.zip, datetime.now(timezone.utc)).ok:
                        manifest.get('screens', {}).pop('solar', None)
                    from ventanal.extras.publish import filter_manifest
                    manifest = filter_manifest(manifest,cfg.cache_dir)
                    full_mv = cfg.battery_full_mv
                    manifest.pop('battery', None)
                    if full_mv is not None: manifest['battery'] = {'full_mv': full_mv}
                    if active(): manifest = ui.security.manifest(manifest, root)
                    body = json.dumps(manifest, ensure_ascii=False, separators=(',', ':')).encode()
                except (OSError, ValueError, KeyError):
                    self.send_error(503); return
                ui.respond(self, 200, body, 'application/json'); return
            if active() and path.startswith('/previews/'):
                ui.handle(self, path); return
            target = root / path.lstrip('/')
            # Reject symbolic links as well as any escape from the publish root.
            if target.resolve() != target:
                self.send_error(404)
                return
            try:
                source = target.open('rb')
            except OSError:
                self.send_error(404)
                return
            with source:
                self.send_response(200)
                suffix = target.suffix
                self.send_header('Content-Type', {'.json': 'application/json', '.vnf': 'application/octet-stream', '.png': 'image/png'}[suffix])
                self.send_header('Content-Length', str(source.seek(0, 2)))
                self.send_header('Cache-Control', 'no-cache' if suffix == '.json' else 'public, max-age=31536000, immutable')
                self.end_headers()
                source.seek(0)
                try:
                    while chunk := source.read(65536):
                        self.wfile.write(chunk)
                except (ConnectionError, TimeoutError):
                    pass  # A disconnected display can retry on its next poll.

        def do_HEAD(self):
            if self.transport(): self.send_error(405 if active() else 501)

        def do_POST(self):
            if not self.transport(): return
            if ui is None:
                self.send_error(501)
                return
            ui.handle(self, unquote(urlsplit(self.path).path), post=True)

        def log_request(self, code='-', size='-'):
            logging.getLogger(__name__).info('HTTP %s %s %s', self.command,
                                            urlsplit(self.path).path, code)

        def log_message(self, fmt, *args):
            pass  # Error text may contain the raw request target.

    class TLSServer(ThreadingHTTPServer):
        def get_request(self):
            conn, addr = super().get_request()
            conn.settimeout(5)
            try:
                secured = ui.security.context.wrap_socket(conn, server_side=True)
                secured.settimeout(30)
                return secured, addr
            except Exception:
                conn.close(); raise

    class Server(ThreadingHTTPServer):
        tls_server = None
        tls_thread = None

        def server_close(self):
            if self.tls_server is not None:
                self.tls_server.shutdown(); self.tls_server.server_close()
                self.tls_thread.join()
            if ui is not None:
                ui.cleanup_stop.set()
            super().server_close()

    server = Server((host, port), Handler)
    server.ui = ui
    if ui is not None:
        from ventanal.config import load_config
        cfg = load_config(config_path)
        tls_lock = RLock()
        def start_tls():
            with tls_lock:
                if server.tls_server is None:
                    # Bind before creating private material so port errors cannot
                    # leave password setup partially activated.
                    tls = TLSServer((https_host if https_host is not None else cfg.https_host,
                                     https_port if https_port is not None else cfg.https_port), Handler)
                    try: ui.security.enable()
                    except Exception:
                        tls.server_close(); raise
                    server.tls_server = tls
                    server.tls_thread = Thread(target=tls.serve_forever, daemon=True)
                    server.tls_thread.start()
        ui.enable_security = start_tls
        if ui.security.ready: start_tls()
    return server
