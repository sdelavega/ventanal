# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Stephen de la Vega

"""A read-only HTTP surface limited to published display assets."""

from contextlib import ExitStack
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from ipaddress import IPv4Address, IPv6Address
import logging
import json
import sqlite3
import ssl
import socket
from threading import BoundedSemaphore, Thread, RLock
from pathlib import Path
import re
from urllib.parse import unquote, urlsplit, parse_qs
from datetime import datetime, timezone
from ventanal.config import load_config, normalize_hostname
from ventanal.manifest import (MAX_SERVED_MANIFEST_BYTES, encode_board_manifest,
                               omit_board_credits, select_epaper_encoding,
                               select_lcd_encoding)
from ventanal.owner import SkipQueue, parse_skips


MAX_TLS_CONNECTIONS = 32
TLS_IDLE_TIMEOUT = 10
DEVICE_PATH = re.compile(r'/devices/([a-z0-9-]{1,15})/')
FRAME_ID_SUFFIX = re.compile(r'(?:^|-)([0-9a-f]{16})$')
BYTE_RANGE = re.compile(r'bytes=([0-9]*)-([0-9]*)', re.IGNORECASE)
log = logging.getLogger(__name__)


def asset_range(headers, etag, total):
    """Return an HTTP status, starting offset and response body length."""
    ranges = headers.get_all('Range', [])
    validators = headers.get_all('If-Range', [])
    if len(ranges) != 1 or (validators and (len(validators) != 1 or validators[0].strip() != etag)):
        return 200, 0, total
    match = BYTE_RANGE.fullmatch(ranges[0].strip())
    if not match or not any(match.groups()):
        return 200, 0, total

    def offset(digits):
        # Larger values have the same effect on the response. Avoid converting
        # arbitrarily long integers supplied by an HTTP client.
        digits = digits.lstrip('0')
        return total + 1 if len(digits) > len(str(total)) else int(digits or '0')

    first, last = match.groups()
    if first:
        start = offset(first)
        end = min(total - 1, offset(last)) if last else total - 1
    else:
        start = max(0, total - offset(last))
        end = total - 1
    if start > end:
        return 416, 0, 0
    return 206, start, end - start + 1


def allowed_host(headers, names):
    hosts = headers.get_all('Host', [])
    if len(hosts) != 1:
        return False
    authority = re.fullmatch(r'(\[[0-9a-fA-F:.]+\]|[^:\[\]]+)(?::([0-9]{1,5}))?', hosts[0].strip(' \t'))
    if not authority:
        return False
    name, port = authority.groups()
    if port is not None and not 1 <= int(port) <= 65535:
        return False
    try:
        if name.startswith('['):
            IPv6Address(name[1:-1])
        else:
            IPv4Address(name)
    except ValueError:
        return normalize_hostname(name) in names
    return True


def make_server(out_dir: Path, host: str, port: int, *, cache_dir: Path | None = None, config_path: Path | None = None, rerender=None, https_host=None, https_port=None) -> ThreadingHTTPServer:
    root = out_dir.resolve()

    from ventanal.status import BoardStore, parse_board
    if config_path is not None:
        from ventanal.ui import UI
        cfg = load_config(config_path)
        ui = UI(config_path, cache_dir, rerender)
        board_store = ui.store
    else:
        ui = None
        board_store = BoardStore(cache_dir) if cache_dir is not None else None
    skip_queue = SkipQueue(cache_dir) if cache_dir is not None else None
    hostname = socket.gethostname().removesuffix('.')
    names = {'localhost', normalize_hostname(hostname), normalize_hostname(f'{hostname}.local')}
    names.discard(None)
    if ui is not None:
        names.update(cfg.allowed_hosts)

    def active():
        return ui is not None and ui.auth.mode == 'password' and ui.security.ready

    def solar_allowed(cfg) -> bool:
        """Request-time ham gate, shared by the manifest routes: solar may
        only be advertised while the licence check passes. Everything else
        (no call sign, expired or inactive licence, unreadable database)
        fails closed — a stale published entry must not keep advertising."""
        from ventanal.solar.licence import verify
        if not cfg.ham_call_sign:
            return False
        try:
            return verify(cfg.cache_dir / 'fcc.sqlite3', cfg.ham_call_sign,
                           cfg.zip, datetime.now(timezone.utc)).ok
        except Exception:
            return False

    class Handler(BaseHTTPRequestHandler):
        @property
        def protocol_version(self):
            return 'HTTP/1.1' if active() and isinstance(self.connection, ssl.SSLSocket) else 'HTTP/1.0'

        def parse_request(self):
            if not super().parse_request():
                return False
            if not allowed_host(self.headers, names | ui.security.dns_names if ui is not None else names):
                self.send_error(400, 'Invalid Host header')
                return False
            return True

        def handle_expect_100(self):
            # Defer the interim response until the route is ready to read the body.
            return True

        def continue_body(self):
            if self.protocol_version == 'HTTP/1.1' and self.request_version == 'HTTP/1.1' and self.headers.get('Expect', '').lower() == '100-continue':
                super().handle_expect_100()

        def transport(self):
            self.proof_key = None
            self.pairing_id = None
            if not active(): return True
            path = unquote(urlsplit(self.path).path)
            board = (path == '/manifest.json' or path.startswith(('/frames/', '/devices/', '/firmware/')))
            if not isinstance(self.connection, ssl.SSLSocket):
                if board or path.startswith('/previews/'):
                    self.send_error(403)
                else:
                    # Never use an untrusted Host header to construct redirects.
                    hostname = socket.gethostname() if host in ('0.0.0.0', '::') else host
                    authority = f'[{hostname}]' if ':' in hostname else hostname
                    ui.respond(self, 308, location=f'https://{authority}:{server.tls_server.server_address[1]}'+self.path)
                return False
            if board:
                profile = DEVICE_PATH.match(path)
                firmware = path.startswith('/firmware/')
                devices = len(self.headers.get_all('X-Ventanal-Device', []))
                if (len(self.headers.get_all('Authorization', [])) == 1
                        and len(self.headers.get_all('X-Ventanal-Counter', [])) == 1
                        and (devices == (1 if profile else 0) or (firmware and devices == 1))
                        and (profile or not path.startswith('/devices/'))):
                    self.proof_key = ui.security.authenticate(
                        self.command, self.path, self.headers, profile[1] if profile else None, firmware=firmware)
                if self.proof_key is None:
                    log.info('Board proof rejected')
                    self.send_error(403)
                    return False
                self.pairing_id = self.headers.get('X-Ventanal-Device')
                log.info('Board proof accepted')
            return True

        def do_GET(self):
            if not self.transport(): return
            path = unquote(urlsplit(self.path).path)
            firmware = None
            if path.startswith('/firmware/'):
                if ui is None or (self.proof_key is None and ui.auth.mode != 'none'):
                    self.send_error(404)
                    return
                with ExitStack() as stack:
                    try:
                        image = stack.enter_context(ui.firmware.open_image(path.removeprefix('/firmware/')))
                    except (OSError, sqlite3.Error):
                        self.send_error(503)
                        return
                    if image is None:
                        self.send_error(404)
                        return
                    source, sha256 = image
                    self.send_asset(source, 'application/octet-stream', 'no-store', f'"{sha256}"')
                return
            if ui is not None and not re.fullmatch(r'/(?:manifest\.json|frames/.*|previews/.*|devices/[a-z0-9-]+(?:/(?:manifest\.json|frames/.*|previews/.*))?)', path):
                ui.handle(self, path)
                return
            if not re.fullmatch(r'/(?:manifest\.json|frames/[A-Za-z0-9_-]+\.vn[fz]|previews/[A-Za-z0-9_-]+\.png|devices/[a-z0-9-]+/(?:manifest\.json|frames/[A-Za-z0-9_-]+\.vnr))', path):
                self.send_error(404)
                return
            device_manifest = re.fullmatch(r'/devices/([a-z0-9-]+)/manifest\.json', path)
            report = {}
            if device_manifest or path == '/manifest.json':
                query = parse_qs(urlsplit(self.path).query, keep_blank_values=True)
                report = parse_board(query)
            if cache_dir is not None and (path == '/manifest.json' or device_manifest):
                values = query.get('skips', [])
                if values:
                    try:
                        skip_queue.enqueue(parse_skips(','.join(values)), datetime.now(timezone.utc))
                    except sqlite3.Error:
                        logging.getLogger(__name__).warning('Unable to queue board skips')
                        self.send_error(503)
                        return
                try:
                    if device_manifest:
                        board_store.checkin_lcd(self.pairing_id or device_manifest[1],
                                               self.client_address[0], report, timeout=1)
                    else:
                        board_store.checkin(self.client_address[0], report, timeout=1)
                    if ui is not None:
                        # Share the identity locks with stale approval cleanup.
                        with ui.auth.lock, ui.security.lock:
                            device = ui.firmware_identity(self.proof_key, self.pairing_id)
                            if device is not None and (not device_manifest or report.get('board') == 'lcd43b'):
                                firmware = ui.firmware.checkin(
                                    report.get('board'), device, report.get('fw'), report.get('ota'),
                                    report.get('ota_approval'),
                                )
                except sqlite3.Error:
                    logging.getLogger(__name__).warning('Unable to record board check-in')
            if path == '/manifest.json':
                target = root / 'manifest.json'
                if ui is None and target.resolve() != target:
                    self.send_error(404); return
                try:
                    with target.open(encoding='utf-8') as f:
                        manifest = json.load(f)
                    if ui is not None:
                        cfg = load_config(config_path)
                        from ventanal.solar.licence import verify
                        if not verify(cfg.cache_dir/'fcc.sqlite3', cfg.ham_call_sign, cfg.zip, datetime.now(timezone.utc)).ok:
                            manifest.get('screens', {}).pop('solar', None)
                        from ventanal.extras.publish import filter_manifest
                        manifest = filter_manifest(manifest,cfg.cache_dir)
                        full_mv = cfg.battery_full_mv
                        manifest.pop('battery', None)
                        if full_mv is not None: manifest['battery'] = {'full_mv': full_mv}
                    omit_board_credits(manifest)
                    select_epaper_encoding(manifest, report)
                    sign = None
                    if self.proof_key is not None:
                        sign = lambda document: ui.security.manifest(document, root, key=self.proof_key)
                    body = encode_board_manifest(manifest, firmware, sign=sign)
                except FileNotFoundError:
                    self.send_error(404 if ui is None else 503); return
                except (OSError, ValueError, KeyError, TypeError, AttributeError):
                    self.send_error(503); return
                if len(body) > MAX_SERVED_MANIFEST_BYTES:
                    log.warning('Oversize manifest (%d bytes, limit %d); returning 503',
                                len(body), MAX_SERVED_MANIFEST_BYTES)
                    self.send_response(503)
                    self.send_header('Content-Length', '0')
                    self.end_headers()
                    return
                if ui is not None:
                    ui.respond(self, 200, body, 'application/json')
                else:
                    self.send_response(200)
                    self.send_header('Content-Type', 'application/json')
                    self.send_header('Content-Length', str(len(body)))
                    self.send_header('Cache-Control', 'no-cache')
                    self.end_headers()
                    try:
                        self.wfile.write(body)
                    except (ConnectionError, TimeoutError):
                        pass
                return
            if device_manifest:
                # Device manifests re-check the ham licence on every
                # request, fail closed like the main manifest above: a
                # solar screen published while authorised must stop being
                # advertised once the licence expires or the call sign is
                # removed from the configuration.
                target = root / path.lstrip('/')
                # The same path-resolution/symlink rejection as the shared
                # route below: a symlinked manifest (or parent directory)
                # must never open a file outside the publication root.
                if target.resolve() != target:
                    self.send_error(404)
                    return
                try:
                    with target.open() as f:
                        manifest = json.load(f)
                    if not isinstance(manifest, dict):
                        raise ValueError('expected a JSON object')
                except (OSError, ValueError):
                    self.send_error(404); return
                screens = manifest.get('screens')
                if ui is not None and isinstance(screens, dict) and not solar_allowed(load_config(config_path)):
                    screens.pop('solar', None)
                try:
                    select_lcd_encoding(manifest, report)
                    sign = None
                    if self.proof_key is not None:
                        omit_board_credits(manifest)
                        sign = lambda document: ui.security.manifest(
                            document, root, key=self.proof_key, profile=device_manifest[1],
                            device=self.pairing_id,
                        )
                    body = encode_board_manifest(manifest, firmware, sign=sign)
                except (OSError, ValueError, KeyError, TypeError, AttributeError):
                    self.send_error(503); return
                if len(body) > MAX_SERVED_MANIFEST_BYTES:
                    self.send_error(503); return
                if ui is not None:
                    ui.respond(self, 200, body, 'application/json')
                else:
                    self.send_response(200)
                    self.send_header('Content-Type', 'application/json')
                    self.send_header('Content-Length', str(len(body)))
                    self.send_header('Cache-Control', 'no-cache')
                    self.end_headers()
                    try:
                        self.wfile.write(body)
                    except (ConnectionError, TimeoutError):
                        pass
                return
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
            suffix = target.suffix
            kind = {'.json': 'application/json', '.png': 'image/png',
                    '.vnf': 'application/octet-stream', '.vnz': 'application/octet-stream',
                    '.vnr': 'application/octet-stream'}[suffix]
            cache_control = 'no-cache' if suffix == '.json' else 'public, max-age=31536000, immutable'
            etag = None
            if suffix in ('.vnf', '.vnz', '.vnr'):
                ident = FRAME_ID_SUFFIX.search(target.stem)
                # A fallback stem cannot match a manifest ID, so devices fetch the full frame.
                etag = f'"{ident[1] if ident else target.stem}"'
            self.send_asset(source, kind, 'no-store' if self.proof_key is not None else cache_control, etag)

        def send_asset(self, source, kind, cache_control, etag=None):
            with source:
                total = source.seek(0, 2)
                status, start, remaining = asset_range(self.headers, etag, total) if etag else (200, 0, total)
                self.send_response(status)
                self.send_header('Content-Type', kind)
                self.send_header('Content-Length', str(remaining))
                self.send_header('Cache-Control', cache_control)
                if etag is not None:
                    self.send_header('Accept-Ranges', 'bytes')
                    self.send_header('ETag', etag)
                if status == 206:
                    self.send_header('Content-Range', f'bytes {start}-{start + remaining - 1}/{total}')
                elif status == 416:
                    self.send_header('Content-Range', f'bytes */{total}')
                self.end_headers()
                source.seek(start)
                try:
                    while remaining and (chunk := source.read(min(65536, remaining))):
                        self.wfile.write(chunk)
                        remaining -= len(chunk)
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
            log.info('HTTP %s %s %s', self.command, urlsplit(self.path).path, code)

        def log_message(self, fmt, *args):
            pass  # Error text may contain the raw request target.

    class TLSServer(ThreadingHTTPServer):
        def __init__(self, *args):
            self.connection_slots = BoundedSemaphore(MAX_TLS_CONNECTIONS)
            super().__init__(*args)

        def process_request(self, conn, addr):
            # Reserve capacity before starting a worker, including keep-alive time.
            if not self.connection_slots.acquire(blocking=False):
                log.warning('Dropping TLS connection: all %d slots are busy', MAX_TLS_CONNECTIONS)
                self.shutdown_request(conn)
                return
            try:
                super().process_request(conn, addr)
            except BaseException:
                self.connection_slots.release()
                raise

        def process_request_thread(self, conn, addr):
            try:
                super().process_request_thread(conn, addr)
            finally:
                self.connection_slots.release()

        def finish_request(self, conn, addr):
            # ThreadingHTTPServer calls finish_request in the request worker.
            conn.settimeout(5)
            try:
                secured = ui.security.context.wrap_socket(conn, server_side=True)
            except OSError:
                return
            try:
                secured.settimeout(TLS_IDLE_TIMEOUT)
                super().finish_request(secured, addr)
            finally:
                self.shutdown_request(secured)

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
