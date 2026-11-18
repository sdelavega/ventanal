# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Stephen de la Vega
from datetime import datetime, timezone
from pathlib import Path
from http.client import HTTPConnection, HTTPSConnection
import ssl
from test_security import proof
from threading import Thread
from urllib.parse import urlencode
import json
import re
import pytest
from ventanal.auth import Auth
from ventanal.status import BoardStore, next_wake, parse_board, showing
from ventanal.settings import save_settings
from ventanal.config import load_config
from ventanal.serve import make_server

EXAMPLE = Path(__file__).parents[1] / 'ventanal.example.toml'


def test_auth_storage_sessions_and_password_change(tmp_path):
    a = Auth(tmp_path)
    assert a.mode == 'unset'
    session, cookie = a.issue(False)
    assert a.session(cookie) == session
    assert a.session(cookie + 'x') is None
    assert a.session(a.sign({**session, 'exp': 0})) is None
    a.set_password('correct horse', 'correct horse')
    assert a.mode == 'password' and a.verify('correct horse') and not a.verify('wrong')
    assert a.session(cookie) is None  # setup invalidates anonymous sessions
    assert (tmp_path / 'auth.json').stat().st_mode & 0o777 == 0o600
    assert (tmp_path / 'session.secret').stat().st_mode & 0o777 == 0o600
    assert 'correct horse' not in (tmp_path / 'auth.json').read_text()
    assert Auth(tmp_path).verify('correct horse')
    with pytest.raises(ValueError): a.set_password('a', 'b')
    a.none()
    assert Auth(tmp_path).mode == 'none'


def test_throttle(tmp_path):
    a = Auth(tmp_path)
    for _ in range(10): a.failed('client', now=100)
    assert not a.allowed('client', now=101)
    assert a.allowed('other', now=101)
    assert a.allowed('client', now=161)


def test_board_validation_and_persistence(tmp_path, caplog):
    assert parse_board({}) == {}
    valid = {'bat': ['95'], 'screen': ['art'], 'frame': ['0123456789abcdef'], 'fw': ['1.0.0']}
    values = parse_board(valid)
    assert values == {'bat': 95, 'screen': 'art', 'frame': '0123456789abcdef', 'fw': '1.0.0'}
    for key, val in [('bat', '-1'), ('bat', '101'), ('bat', '01'), ('screen', 'x'), ('frame', '../x'), ('fw', '<script>')]:
        assert key not in parse_board({key: [val]})
    assert parse_board({'bat': ['1', '2']}) == {}
    assert 'malformed' in caplog.text
    s = BoardStore(tmp_path)
    s.checkin('192.0.2.1', values)
    assert BoardStore(tmp_path).latest()['bat'] == 95
    assert s.recent()[0]['kind'] == 'check-in'


def test_schedule_and_showing():
    cfg = load_config(EXAMPLE)
    for now, expected in [('2026-11-16T22:59:00-05:00','2026-11-17T06:00:00-05:00'), ('2026-11-16T12:01:00-05:00','2026-11-16T12:30:00-05:00')]:
        assert next_wake(cfg, datetime.fromisoformat(now)).isoformat() == expected
    m = {'screens': {'art': {'id': 'a'}}, 'spares': [{'id': 'b'}]}
    assert showing({'screen': 'art', 'frame': 'b'}, m) == 'Spare 1 of 3'
    assert showing({'screen': 'art', 'frame': 'a'}, m) == 'Main art'
    assert showing({'screen': 'weather'}, m) == 'Weather'
    assert showing({'screen': 'art', 'frame': 'old'}, m) == 'Art (previous frame)'


def test_settings_roundtrip_and_backups(tmp_path):
    path = tmp_path / 'settings.toml'
    path.write_text(EXAMPLE.read_text())
    for i in range(12):
        save_settings(path, {'schedule.weather_interval_minutes': str(30+i)}, tmp_path / 'backups')
    assert '# Quiet hours' in path.read_text()
    assert load_config(path).weather_interval_minutes == 41
    assert len(list((tmp_path / 'backups').glob('*.toml'))) == 10
    before = path.read_bytes()
    with pytest.raises(ValueError, match='location.zip'):
        save_settings(path, {'location.zip': 'bad'}, tmp_path / 'backups')
    assert path.read_bytes() == before


@pytest.fixture
def web(tmp_path):
    path = tmp_path / 'settings.toml'
    path.write_text(EXAMPLE.read_text().replace('/var/lib/ventanal', str(tmp_path))
                    .replace('/var/cache/ventanal', str(tmp_path/'cache'))
                    .replace('[server]', '[server]\nallowed_hosts = ["Living_Room.Example.Test."]'))
    srv = make_server(tmp_path, '127.0.0.1', 0, cache_dir=tmp_path/'cache', config_path=path, rerender=lambda: None, https_port=0)
    t = Thread(target=srv.serve_forever, daemon=True); t.start()
    cookie = ''
    def request(url='/', data=None, csrf=True):
        nonlocal cookie
        secure=srv.ui.security.ready and srv.ui.auth.mode=='password'
        c = HTTPSConnection('127.0.0.1',srv.tls_server.server_address[1],context=ssl.create_default_context(cadata=srv.ui.security.certificate)) if secure else HTTPConnection(*srv.server_address)
        headers = {'Cookie': cookie}
        if secure and url.startswith('/manifest.json'):
            headers.update(proof(srv.ui.security.key,url,srv.ui.security.state['counter']+1))
        if data is not None:
            headers['Content-Type'] = 'application/x-www-form-urlencoded'
        c.request('GET' if data is None else 'POST', url, None if data is None else urlencode(data, doseq=True), headers)
        r = c.getresponse(); body = r.read().decode(errors='replace'); status = r.status
        if r.getheader('Set-Cookie'): cookie = r.getheader('Set-Cookie').split(';')[0]
        c.close()
        token = re.search(r'name="csrf" value="([^"]+)"', body)
        return status, body, token.group(1) if token else ''
    yield request, srv, tmp_path
    srv.shutdown(); srv.server_close(); t.join()


def test_routes_setup_skip_and_csrf(web):
    r, srv, root = web
    status, body, token = r()
    assert status == 200 and 'Choose a password' in body
    setup_token = srv.ui.auth.issue_setup_token()
    assert setup_token not in body and 'name="setup_token"' in body
    assert 'minlength="10"' in body
    assert r('/setup', {'password':'test secret', 'confirm':'test secret'})[0] == 403
    assert r('/setup/skip', {'csrf':token})[0] == 403
    assert r('/setup/skip', {'csrf':token, 'setup_token':'wrong'})[0] == 403
    assert r('/setup/skip', {'csrf':token, 'setup_token':setup_token})[0] == 200
    assert srv.ui.auth.mode == 'unset'
    assert r('/setup/skip', {'csrf':token, 'setup_token':setup_token})[0] == 403
    assert r('/setup/confirm-skip', {'csrf':token, 'confirm':'yes'})[0] == 303
    assert not Auth(root/'cache').consume_setup_token(setup_token)
    assert r('/setup/skip', {'csrf':token, 'setup_token':setup_token})[0] == 403
    status, body, token = r()
    assert 'NO PASSWORD SET' in body and 'never' in body
    status, _, token = r('/settings')
    assert status == 200
    assert r('/art')[0] == 200 and 'Upload a chart photo' in r('/calibration')[1]
    assert r('/static/ui.css')[0] == 200
    assert r('/static/../auth.json')[0] == 404
    assert r('/settings', {'csrf':'wrong'})[0] == 403
    assert r('/setup/confirm-skip', {'csrf':token})[0] == 400


def test_password_routes_and_change(web):
    r, srv, root = web
    _, _, token = r()
    setup = {'csrf': token, 'password': '0123456789', 'confirm': '0123456789'}
    assert r('/setup', setup)[0] == 403
    assert r('/setup', {**setup, 'setup_token': 'wrong'})[0] == 403
    setup['setup_token'] = srv.ui.auth.issue_setup_token()
    assert r('/setup', {**setup, 'confirm': 'different'})[0] == 400
    assert r('/setup', setup)[0] == 403
    setup['setup_token'] = srv.ui.auth.issue_setup_token()
    assert r('/setup', {**setup, 'password': 'ninechars', 'confirm': 'ninechars'})[0] == 400
    assert srv.ui.auth.mode == 'unset' and srv.tls_server is None
    setup['setup_token'] = srv.ui.auth.issue_setup_token()
    assert r('/setup', setup)[0] == 303
    assert Auth(root/'cache').issue_setup_token() is None
    _, _, token = r('/login')
    assert r('/login', {'csrf':token,'password':setup['password']})[0] == 303
    _, body, token = r()
    assert 'NO PASSWORD SET' not in body
    assert r('/logout', {'csrf':token})[0] == 303
    assert r('/settings')[0] == 303
    _, _, token = r('/login')
    assert r('/login', {'csrf':token, 'password':'wrong'})[0] == 401
    srv.ui.auth.attempts.clear()
    assert r('/login', {'csrf':token, 'password':setup['password']})[0] == 303
    _, _, token = r('/password')
    assert r('/password', {'csrf':token,'current':'wrong','password':'new password','confirm':'new password'})[0] == 400
    assert r('/password', {'csrf':token,'current':setup['password'],'password':'new','confirm':'new'})[0] == 400
    assert r('/password', {'csrf':token,'current':setup['password'],'password':'new password','confirm':'new password'})[0] == 303
    assert srv.ui.auth.verify('new password')
    (root/'manifest.json').write_text('{}')
    assert r('/manifest.json?bat=90&screen=weather&frame=0123456789abcdef&fw=1.0.0')[0] == 200
    assert BoardStore(root/'cache').latest()['bat'] == 90


def test_setup_token_cli_in_open_mode(web, capsys):
    from ventanal.cli import main
    r, srv, root = web
    args = ['setup-token', '--config', str(root/'settings.toml')]
    main(args)
    first_token = capsys.readouterr().out.strip()
    _, _, csrf = r('/setup')
    assert r('/setup/skip', {'csrf':csrf, 'setup_token':first_token})[0] == 200
    assert r('/setup/confirm-skip', {'csrf':csrf, 'confirm':'yes'})[0] == 303
    status, body, csrf = r('/setup')
    assert status == 200 and 'name="setup_token"' in body
    setup = {'csrf':csrf, 'password':'test secret', 'confirm':'test secret'}
    assert r('/setup', setup)[0] == 403
    assert r('/setup', {**setup, 'setup_token':first_token})[0] == 403
    assert srv.ui.auth.mode == 'none'
    main(args)
    stale = capsys.readouterr().out.strip()
    main(args)
    fresh = capsys.readouterr().out.strip()
    assert r('/setup', {**setup, 'setup_token':stale})[0] == 403
    assert r('/setup', {**setup, 'setup_token':fresh})[0] == 303
    main(args)
    assert capsys.readouterr().out == 'Setup is already complete.\n'


def test_auth_rejects_incomplete_session_secret(tmp_path):
    (tmp_path/'session.secret').touch()
    with pytest.raises(ValueError, match='Invalid session secret'):
        Auth(tmp_path)


def test_host_allowlist_in_open_mode_and_on_tls(web):
    import socket
    _, srv, _ = web

    def request(connection, method, path, hosts):
        try:
            connection.putrequest(method, path, skip_host=True)
            for host in hosts:
                connection.putheader('Host', host)
            connection.endheaders()
            response = connection.getresponse()
            response.read()
            return response.status
        finally:
            connection.close()

    def http(method, path, *hosts):
        return request(HTTPConnection(*srv.server_address, timeout=2), method, path, hosts)

    assert http('GET', '/', 'unknown.example.test') == 400
    srv.ui.auth.none()
    hostname = socket.gethostname().removesuffix('.')
    for host in ('127.0.0.1:8765', '[::1]:8765', 'LOCALHOST', hostname, hostname+'.local', 'living_room.example.test:8765'):
        assert http('GET', '/settings', host) == 200
    for hosts in ((), ('localhost', 'localhost'), ('localhost, unknown.example.test',),
                  ('localhost:65536',), ('localhost:',), ('http://localhost',), ('::1',)):
        assert http('GET', '/manifest.json', *hosts) == 400
    assert http('POST', '/settings', 'unknown.example.test') == 400
    assert http('HEAD', '/manifest.json', 'unknown.example.test') == 400
    assert http('OPTIONS', '/', 'unknown.example.test') == 400

    srv.ui.auth.set_password('test password', 'test password')
    srv.ui.enable_security()
    assert http('GET', '/', 'unknown.example.test') == 400
    assert http('GET', '/', 'localhost') == 308
    context = ssl.create_default_context(cadata=srv.ui.security.certificate)
    for path, host, status in (('/login', 'living_room.example.test', 200),
                               ('/manifest.json', 'unknown.example.test', 400)):
        connection = HTTPSConnection('127.0.0.1', srv.tls_server.server_address[1], timeout=2, context=context)
        assert request(connection, 'GET', path, (host,)) == status


def test_silent_tls_connection_does_not_block_other_clients(web, monkeypatch):
    import socket
    from threading import Event

    _, srv, _ = web
    srv.ui.auth.set_password('test secret', 'test secret')
    srv.ui.enable_security()
    address = ('127.0.0.1', srv.tls_server.server_address[1])
    context = ssl.create_default_context(cadata=srv.ui.security.certificate)
    started = Event()
    wrap_socket = ssl.SSLContext.wrap_socket

    def observe(self, sock, *args, **kwargs):
        if kwargs.get('server_side'):
            started.set()
        return wrap_socket(self, sock, *args, **kwargs)

    monkeypatch.setattr(ssl.SSLContext, 'wrap_socket', observe)
    with socket.create_connection(address, timeout=2):
        assert started.wait(2)
        conn = HTTPSConnection(*address, timeout=2, context=context)
        try:
            conn.request('GET', '/login')
            response = conn.getresponse()
            assert response.status == 200
            response.read()
        finally:
            conn.close()


def test_tls_connection_cap_covers_keepalive_and_recovers(web, monkeypatch):
    import socket
    from threading import Event

    monkeypatch.setattr('ventanal.serve.MAX_TLS_CONNECTIONS', 1)
    _, srv, _ = web
    srv.ui.auth.set_password('test secret', 'test secret')
    srv.ui.enable_security()
    tls = srv.tls_server
    address = ('127.0.0.1', tls.server_address[1])
    context = ssl.create_default_context(cadata=srv.ui.security.certificate)
    freed = Event()
    process = tls.process_request_thread

    def observe(conn, addr):
        process(conn, addr)
        freed.set()

    monkeypatch.setattr(tls, 'process_request_thread', observe)
    held = HTTPSConnection(*address, timeout=2, context=context)
    try:
        held.request('GET', '/login')
        response = held.getresponse()
        assert response.status == 200 and not response.will_close
        response.read()
        with socket.create_connection(address, timeout=2) as excess:
            assert excess.recv(1) == b''
    finally:
        held.close()
    assert freed.wait(2)
    conn = HTTPSConnection(*address, timeout=2, context=context)
    try:
        conn.request('GET', '/login')
        response = conn.getresponse()
        assert response.status == 200
        response.read()
    finally:
        conn.close()


def test_settings_errors_and_escape(web):
    r, srv, root = web
    _, _, token = r()
    r('/setup/skip', {'csrf':token, 'setup_token':srv.ui.auth.issue_setup_token()})
    r('/setup/confirm-skip', {'csrf':token,'confirm':'yes'})
    _, _, token = r('/settings')
    status, body, _ = r('/settings', {'csrf':token,'location.zip':'<script>'})
    assert status == 400 and 'location.zip:' in body and '<script>' not in body
    assert r('/settings', {'csrf':token,'location.zip':'12345'})[0] == 200


def test_http_rejects_direct_skip_and_throttles(web):
    r, srv, _ = web
    _, _, token = r()
    assert r('/setup/confirm-skip', {'csrf':token,'confirm':'yes'})[0] == 400
    assert srv.ui.auth.mode == 'unset'
    r('/setup', {'csrf':token,'setup_token':srv.ui.auth.issue_setup_token(),'password':'test secret','confirm':'test secret'})
    _, _, token = r()
    r('/logout', {'csrf':token})
    _, _, token = r('/login')
    assert r('/login', {'csrf':token,'password':'wrong'})[0] == 401
    assert r('/login', {'csrf':token,'password':'test secret'})[0] == 429
    assert r('/login', {'csrf':'é','password':'test secret'})[0] == 403
    assert r('/setup')[0] == 303
    assert r('/static/Antonio-Regular.ttf')[0] == 200


def test_settings_seasons_museums_and_secret(tmp_path):
    path=tmp_path/'settings.toml'; path.write_text(EXAMPLE.read_text())
    save_settings(path, {'art.rijksmuseum_key':'private-test-key','art.museums':['aic'],
                         'seasons.count':'1','art.seasons[0].name':'year',
                         'art.seasons[0].start':'01-01','art.seasons[0].end':'12-31',
                         'art.seasons[0].terms':'snow, sky'})
    cfg=save_settings(path, {'art.rijksmuseum_key':''})
    assert cfg.rijksmuseum_key=='private-test-key'
    assert cfg.seasons[0].terms==('snow','sky') and cfg.museums==('aic',)
    assert save_settings(path, {'art.rijksmuseum_key':'', 'art.rijksmuseum_key.clear':'yes'}).rijksmuseum_key == ''
    for values in [{'art.museums':[]},{'schedule.art_slots':'25:00'}, {'schedule.quiet_end':'26:00'},
                   {'weather.here_label':'other'},{'schedule.weather_interval_minutes':'no'},
                   {'location.latitude':'NaN'}, {'seasons.count':'0'}]:
        before=path.read_bytes()
        with pytest.raises(ValueError): save_settings(path, values)
        assert path.read_bytes()==before


def test_preview_uses_palette_and_cache(tmp_path):
    from dataclasses import replace
    import numpy as np
    from ventanal.frame import encode_frame, frame_id
    from ventanal.status import preview
    from PIL import Image
    cfg=replace(load_config(EXAMPLE), output_dir=tmp_path, cache_dir=tmp_path/'cache')
    frame=encode_frame(np.ones((1200,1600), dtype=np.uint8))
    ident=frame_id(frame)
    (tmp_path/'frames').mkdir()
    (tmp_path/'frames'/'art.vnf').write_bytes(frame)
    entry={'id':ident,'url':'/frames/art.vnf'}
    alias=tmp_path/'alias'; alias.symlink_to(tmp_path, target_is_directory=True)
    cfg=replace(cfg,output_dir=alias)
    target=preview(cfg,entry); stamp=target.stat().st_mtime_ns
    assert Image.open(target).getpixel((0,0))==cfg.panel_rgb[1]
    assert preview(cfg,entry).stat().st_mtime_ns==stamp
    palette=tuple((12,34,56) if i==1 else c for i,c in enumerate(cfg.panel_rgb))
    other=preview(replace(cfg,panel_rgb=palette),entry)
    assert other != target and Image.open(other).getpixel((0,0))==(12,34,56)
    with pytest.raises(ValueError): preview(cfg,{'id':ident,'url':'/frames/../secret'})


@pytest.mark.parametrize(('when','low','expected'),[
    ('2026-11-16T12:01:00-05:00',True,'2026-11-16T14:00:00-05:00'),
    ('2026-11-01T01:45:00-04:00',False,'2026-11-01T01:00:00-05:00'),
    ('2026-03-08T01:45:00-05:00',False,'2026-03-08T03:00:00-04:00'),
])
def test_schedule_dst_and_low(when,low,expected):
    from dataclasses import replace
    cfg=replace(load_config(EXAMPLE),quiet_start='00:00',quiet_end='00:00')
    assert next_wake(cfg,datetime.fromisoformat(when),low).isoformat()==expected


def test_legacy_manifest_records_checkin_and_head_post_do_not(web):
    r,srv,root=web
    (root/'manifest.json').write_text('{}')
    assert r('/manifest.json')[0]==200
    assert srv.ui.store.latest()['ip']=='127.0.0.1'
    with srv.ui.store.connect() as db:
        assert db.execute('SELECT COUNT(*) FROM board_checkins').fetchone()[0]==1
    c=HTTPConnection(*srv.server_address)
    c.request('HEAD','/manifest.json?bat=1'); c.getresponse().read(); c.close()
    r('/manifest.json?bat=1', {'csrf':'x'})
    assert 'bat' not in srv.ui.store.latest()


def test_settings_rows_and_background_error(web):
    r,srv,root=web
    _,_,token=r()
    r('/setup/skip',{'csrf':token, 'setup_token':srv.ui.auth.issue_setup_token()}); r('/setup/confirm-skip',{'csrf':token,'confirm':'yes'})
    _,_,token=r('/settings')
    before=(root/'settings.toml').read_bytes()
    status,body,_=r('/settings', {'csrf':token,'action':'rows','seasons.count':'6'})
    assert status==200 and 'art.seasons[5].name' in body
    assert (root/'settings.toml').read_bytes()==before
    assert r('/settings',{'csrf':token,'action':'rows','seasons.count':'bad'})[0]==400
    def fail(): raise RuntimeError('private failure details')
    srv.ui.rerender=fail
    srv.ui.render_delay=.01
    assert r('/settings',{'csrf':token,'location.zip':'12345'})[0]==200
    import time
    for _ in range(100):
        if not srv.ui.rendering: break
        time.sleep(.01)
    assert not srv.ui.rendering and 'incomplete' in srv.ui.render_result
    assert 'private failure' not in r('/settings')[1]
    assert any(row['kind']=='tick error' for row in srv.ui.store.recent())


def test_save_during_render_queues_another_pass(web):
    from threading import Event
    _,srv,_=web
    started=Event(); release=Event(); finished=Event(); calls=[]
    def render():
        calls.append(1)
        if len(calls)==1:
            started.set(); assert release.wait(3)
        else: finished.set()
    srv.ui.rerender=render
    srv.ui.start_render(); assert started.wait(3)
    srv.ui.start_render(); release.set()
    assert finished.wait(3)
    assert len(calls)==2


def test_cookie_flags_and_no_secret_in_forms(web):
    _,srv,_=web
    c=HTTPConnection(*srv.server_address); c.request('GET','/')
    response=c.getresponse(); body=response.read().decode()
    cookie=response.getheader('Set-Cookie'); c.close()
    for flag in ('HttpOnly','SameSite=Strict','Path=/','Max-Age=2592000'): assert flag in cookie
    forms=re.findall(r'<form.*?</form>',body,re.S)
    assert forms and all('name="csrf"' in f for f in forms)
    srv.ui.auth.set_password('fixture password','fixture password')
    stored=json.loads((srv.ui.auth.path).read_text())
    assert len(bytes.fromhex(stored['salt']))==16
    import hashlib
    assert hashlib.scrypt(b'fixture password',salt=bytes.fromhex(stored['salt']),n=2**14,r=8,p=1).hex()==stored['hash']


def test_previous_spare_identity_stays_resolvable(tmp_path):
    store=BoardStore(tmp_path)
    store.role('a'*16,'spare1')
    store.role('b'*16,'spare1')
    assert store.frame_role('a'*16)=='spare1'
    assert showing({'screen':'art','frame':'a'*16},{},store.frame_role('a'*16))=='Spare 2 of 3 (previous frame)'


def test_settings_nonnumeric_count_returns_validation_page(web):
    r, srv, root = web
    srv.ui.auth.none()
    _, _, token = r('/settings')
    before = (root / 'settings.toml').read_bytes()
    status, body, _ = r('/settings', {'csrf': token, 'seasons.count': 'no'})
    assert status == 400
    assert 'art.seasons: expected 1–50 rows' in body
    assert (root / 'settings.toml').read_bytes() == before
    assert not list((root / 'backups').glob('*.toml'))


def test_settings_human_labels_and_managed_values(web):
    r, srv, root = web
    srv.ui.auth.none()
    _, body, token = r('/settings')
    for label in ('ZIP code', 'Map caption', 'NWS contact email', 'Paris Musées API token',
                  'Radar radius (miles)', 'Sleep hours — start', 'Art change times',
                  "Don&#x27;t repeat art for (days)", 'Panel colours', 'Ignored artworks'):
        assert label in body
    assert '<details' in body and '<summary>Advanced</summary>' in body
    before = load_config(root / 'settings.toml')
    assert r('/settings', {'csrf': token, 'art.ignored': 'met:123', 'panel.black': '#ffffff'})[0] == 200
    after = load_config(root / 'settings.toml')
    assert after.ignored == before.ignored and after.panel_rgb == before.panel_rgb


def test_activity_collapses_repeated_interleaved_pairs(tmp_path):
    store = BoardStore(tmp_path)
    for _ in range(30):
        store.event('publish', 'weather abc')
        store.checkin('192.0.2.1', {'bat': 90, 'screen': 'art'})
    rows = store.recent()
    assert len(rows) == 2
    assert all(row['count'] == 30 and row['first_time'] <= row['time'] for row in rows)
    store.checkin('192.0.2.1', {'bat': 89, 'screen': 'art'})
    assert store.recent()[0]['count'] == 1
    for i in range(20): store.event('publish', str(i))
    rows = store.recent()
    assert len(rows) == 12 and rows[0]['detail'] == '19'


def test_request_logging_omits_query_and_secrets(web, caplog):
    import logging
    caplog.set_level(logging.INFO)
    r, _, _ = web
    r('/missing?secret=do-not-log')
    assert 'GET /missing 404' in caplog.text
    assert 'do-not-log' not in caplog.text and '?secret' not in caplog.text


def test_preview_prunes_to_twenty_newest_files(tmp_path):
    from dataclasses import replace
    import os
    import numpy as np
    from ventanal.frame import encode_frame, frame_id
    from ventanal.status import preview
    cfg = replace(load_config(EXAMPLE), output_dir=tmp_path, cache_dir=tmp_path/'cache')
    directory = cfg.cache_dir/'ui-previews'
    directory.mkdir(parents=True)
    for i in range(25):
        path = directory/f'old-{i:02}.png'
        path.write_bytes(b'old')
        os.utime(path, (i+1, i+1))
    frame = encode_frame(np.ones((1200,1600), dtype=np.uint8))
    (tmp_path/'frames').mkdir()
    (tmp_path/'frames/art.vnf').write_bytes(frame)
    target = preview(cfg, {'id': frame_id(frame), 'url': '/frames/art.vnf'})
    assert target.exists() and len(list(directory.glob('*.png'))) == 20
    assert not (directory/'old-05.png').exists() and (directory/'old-06.png').exists()


def test_security_panel_auth_csrf_reveal_and_rotation(web):
    r,srv,_=web
    assert r('/security')[0]==403  # Requires a password even in open mode.
    _,_,token=r('/setup')
    assert r('/setup',{'csrf':token,'setup_token':srv.ui.auth.issue_setup_token(),'password':'test secret','confirm':'test secret'})[0]==303
    assert r('/security')[0]==303
    _,_,token=r('/login')
    assert r('/login',{'csrf':token,'password':'test secret'})[0]==303
    status,body,token=r('/security')
    assert status==200 and 'Security ON' in body
    key=srv.ui.security.key.hex(); cert=srv.ui.security.certificate
    assert key not in body and 'BEGIN CERTIFICATE' not in body
    assert r('/security',{'csrf':'bad','action':'reveal'})[0]==403
    status,body,_=r('/security',{'csrf':token,'action':'reveal'})
    assert status==200 and key in body and 'BEGIN CERTIFICATE' in body
    assert r('/security',{'csrf':token,'action':'repair'})[0]==400
    assert srv.ui.security.key.hex()==key
    assert r('/security',{'csrf':token,'action':'repair','confirm':'yes'})[0]==200
    assert srv.ui.security.key.hex()!=key
    assert r('/security',{'csrf':token,'action':'regenerate','confirm':'yes'})[0]==200
    assert srv.ui.security.certificate!=cert
    assert r('/security')[0]==200  # New certificate serves successfully, session retained.


def test_existing_config_displays_https_default(web):
    r,srv,_=web
    srv.ui.auth.none()
    text=srv.ui.config_path.read_text()
    srv.ui.config_path.write_text(text.replace('https_listen = "0.0.0.0:8766"\n',''))
    status,body,_=r('/settings')
    assert status==200 and 'name="server.https_listen" type="text" value="0.0.0.0:8766"' in body


def test_existing_password_requires_explicit_activation_and_tls_login(web):
    r,srv,_=web
    srv.ui.auth.set_password('existing password','existing password')
    assert not srv.ui.security.ready and srv.tls_server is None
    _,_,token=r('/login')
    assert r('/login',{'csrf':token,'password':'existing password'})[0]==303
    status,body,token=r('/security')
    assert status==200 and 'activation required' in body
    assert r('/security',{'csrf':token,'action':'enable'})[0]==400
    assert not srv.ui.security.ready
    assert r('/security',{'csrf':token,'action':'regenerate','confirm':'yes'})[0]==400
    assert r('/security',{'csrf':token,'action':'repair','confirm':'yes'})[0]==400
    assert r('/security',{'csrf':token,'action':'enable','confirm':'yes'})[0]==303
    assert srv.ui.auth.verify('existing password') and srv.ui.security.ready
    assert r('/security')[0]==303  # HTTP-era sessions invalidated; HTTPS login required.


def test_status_release_versions(web):
    from importlib.metadata import version
    r, srv, root = web
    _, _, token = r()
    r('/setup/skip', {'csrf': token, 'setup_token': srv.ui.auth.issue_setup_token()})
    assert r('/setup/confirm-skip', {'csrf': token, 'confirm': 'yes'})[0] == 303
    srv.ui.store.checkin('192.0.2.1', {'fw': '1.0.0', 'screen': 'weather'})
    status, body, _ = r('/')
    assert status == 200
    assert version('ventanal') == '1.0.0'
    assert 'Server 1.0.0' in body
    assert 'Firmware 1.0.0' in body
