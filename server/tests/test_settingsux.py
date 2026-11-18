# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Stephen de la Vega
import json
import re
import time
import pytest
from test_gui import web, EXAMPLE
from ventanal.config import load_config
from ventanal.settings import save_settings


def test_config_save_is_atomic_and_preserves_metadata(tmp_path, monkeypatch):
    import os
    import stat
    from ventanal.config import load_config

    path = tmp_path / 'settings.toml'
    path.write_text(EXAMPLE.read_text())
    path.chmod(0o640)
    before = path.stat()
    replace = os.replace

    def interrupt(source, destination):
        if destination == path:
            assert load_config(path).clock_24h
            assert not load_config(source).clock_24h
            raise OSError('interrupted before rename')
        return replace(source, destination)

    with monkeypatch.context() as patch:
        patch.setattr(os, 'replace', interrupt)
        with pytest.raises(OSError, match='interrupted'):
            save_settings(path, {'display.clock': '12h'})
    assert load_config(path).clock_24h
    assert path.stat().st_ino == before.st_ino
    assert not list(tmp_path.glob('.settings.toml.*'))
    save_settings(path, {'display.clock': '12h'})
    after = path.stat()
    assert not load_config(path).clock_24h
    assert after.st_ino != before.st_ino
    assert (after.st_uid, after.st_gid, stat.S_IMODE(after.st_mode)) == (
        before.st_uid, before.st_gid, 0o640)


def test_config_fallback_uses_newest_usable_backup(tmp_path):
    from ventanal.config import ConfigError, load_config

    path = tmp_path / 'settings.toml'
    path.write_text(EXAMPLE.read_text())
    save_settings(path, {'display.clock': '12h'})
    save_settings(path, {'display.clock': '24h'})
    backups = tmp_path / 'backups'
    (backups / 'ventanal-99990101T000000.000000Z.toml').write_text('[broken\n')
    (backups / 'ventanal-99990101T000001.000000Z.toml').write_text('[display]\n')
    path.write_bytes(b'\xff')
    fallback = load_config(path)
    assert fallback.config_backup is not None and not fallback.clock_24h
    assert path.read_bytes() == b'\xff'
    assert not save_settings(path, {'display.units': 'metric'}).clock_24h
    assert load_config(path).units == 'metric'
    assert load_config(path).config_backup is None
    archived, = backups.glob('unparseable-*.toml')
    assert archived.read_bytes() == b'\xff'
    assert archived.stat().st_mode & 0o777 == 0o600
    path.write_text(EXAMPLE.read_text().replace('end = "02-29"', 'end = "02-28"'))
    with pytest.raises(ConfigError, match='no season covers 02-29'):
        load_config(path)


@pytest.mark.parametrize('blocked_by', ['owner', 'rename'])
def test_settings_reports_atomic_save_fix_and_preserves_file(web, monkeypatch, blocked_by):
    import errno
    import os
    from ventanal import settings

    request, server, _ = web
    server.ui.auth.none()
    path = server.ui.config_path
    original = path.read_bytes()
    _, _, csrf = request('/settings')
    update = {'csrf': csrf, 'field': 'display.clock', 'display.clock': '12h'}
    owner = path.stat().st_uid
    rename = os.replace

    def denied(source, destination):
        if destination == path:
            raise OSError(errno.EPERM, 'fake rename denial')
        return rename(source, destination)

    with monkeypatch.context() as patch:
        if blocked_by == 'owner':
            patch.setattr(settings.os, 'geteuid', lambda: owner + 1)
            expected = 'sudo chown ventanal:ventanal'
        else:
            patch.setattr(settings.os, 'replace', denied)
            expected = 'systemd ReadWritePaths'
        status, body, _ = request('/settings/field', update)
        assert status == 400 and expected in json.loads(body)['error']
        assert path.read_bytes() == original
        assert not list(path.parent.glob('.settings.toml.*'))
    assert request('/settings/field', update)[0] == 200
    assert not load_config(path).clock_24h


def test_status_shows_config_fallback_until_repaired(web):
    request, server, _ = web
    server.ui.auth.none()
    path = server.ui.config_path
    save_settings(path, {'display.clock': '12h'})
    path.write_bytes(b'[broken\n')
    status, body, _ = request()
    assert status == 200 and 'Using backup ventanal-' in body
    save_settings(path, {'display.clock': '12h'})
    assert 'Using backup' not in request()[1]


def test_config_save_keeps_live_bytes_if_archive_fails(tmp_path, monkeypatch):
    from ventanal import settings

    path = tmp_path / 'settings.toml'
    path.write_text(EXAMPLE.read_text())
    save_settings(path, {})
    broken = b'[broken\n'
    path.write_bytes(broken)
    write = settings.private_write

    def fail_archive(path, data, **kwargs):
        if path.name.startswith('unparseable-'):
            raise OSError('archive failed')
        return write(path, data, **kwargs)

    monkeypatch.setattr(settings, 'private_write', fail_archive)
    with pytest.raises(OSError, match='archive failed'):
        save_settings(path, {'display.clock': '12h'})
    assert path.read_bytes() == broken

@pytest.mark.parametrize('raw,want', [('2200','22:00'),('22:00','22:00'),('22.00','22:00'),('10pm','22:00'),('10 pm','22:00'),('10:00 PM','22:00'),('10 p.m.','22:00'),('6','06:00'),('0600','06:00'),('6:00 am','06:00'),('12am','00:00'),('12PM','12:00')])
def test_time_parser(raw,want):
    from ventanal.settings import parse_time
    assert parse_time(raw) == want

@pytest.mark.parametrize('raw', ['24:00','1260','0pm','13am','-1','6:99','', '1:2', 'noon'])
def test_invalid_time(raw):
    from ventanal.settings import parse_time
    with pytest.raises(ValueError, match='valid time'): parse_time(raw)

def test_normalization_and_backup_session(tmp_path):
    p=tmp_path/'settings.toml'; p.write_text(EXAMPLE.read_text())
    save_settings(p, {'schedule.art_slots':'10pm,0600,6:00 am','location.zip':' 123456789 ', 'display.units':' METRIC '}, browser_session='one', now=100)
    assert '"06:00", "22:00"' in p.read_text()
    assert '12345-6789' in p.read_text()
    save_settings(p, {'display.clock':'24H'}, browser_session='one', now=200)
    assert len(list((tmp_path/'backups').glob('*.toml'))) == 1
    save_settings(p, {'display.clock':'12h'}, browser_session='two', now=201)
    save_settings(p, {'display.clock':'24h'}, browser_session='one', now=3700)
    assert len(list((tmp_path/'backups').glob('*.toml'))) == 3
    assert '# Quiet hours' in p.read_text()

def ready(web):
    r,srv,root=web; srv.ui.auth.none(); _,body,token=r('/settings'); return r,srv,token,body

def test_field_endpoint(web):
    r,srv,token,body=ready(web)
    srv.ui.render_delay=60
    assert 'settings.js' in body and '<noscript>' in body and 'type="time"' in body
    assert 'data-time-chips' in body and 'data-season' in body
    assert r('/settings/field', {'csrf':'wrong','field':'display.units','display.units':'metric'})[0]==403
    status,body,_=r('/settings/field', {'csrf':token,'field':'schedule.quiet_start','schedule.quiet_start':'10 p.m.'})
    assert status==200 and json.loads(body)['saved']
    before=srv.ui.config_path.read_bytes()
    status,body,_=r('/settings/field', {'csrf':token,'field':'schedule.quiet_start','schedule.quiet_start':'25:00'})
    assert status==400 and 'valid time' in json.loads(body)['error']
    assert before==srv.ui.config_path.read_bytes()
    assert r('/settings/field', {'csrf':token,'field':'panel.red','panel.red':'#ffffff'})[0]==400
    assert r('/settings/field', {'csrf':token,'field':'display.units','display.units':'metric','display.clock':'24h'})[0]==400
    assert srv.ui.render_effects == {'schedule'}  # metadata only; no frame tick

def test_field_auth(web):
    r,srv,_=web; srv.ui.auth.set_password('test secret','test secret')
    assert r('/settings/field', {'field':'display.units'})[0]==303

def test_debounce_and_no_overlap(web):
    from threading import Event
    r,srv,token,_=ready(web)
    srv.ui.render_delay=.08
    started=Event(); release=Event(); calls=[]
    def render():
        calls.append(1); started.set(); release.wait(2)
    srv.ui.rerender=render
    for value in ['metric','imperial','metric']:
        assert r('/settings/field', {'csrf':token,'field':'display.units','display.units':value})[0]==200
    assert started.wait(2) and len(calls)==1
    r('/settings/field', {'csrf':token,'field':'display.clock','display.clock':'24h'})
    time.sleep(.12); assert len(calls)==1
    release.set()
    for _ in range(100):
        if len(calls)==2 and not srv.ui.rendering: break
        time.sleep(.02)
    assert len(calls)==2 and not srv.ui.rendering

def test_grouped_season_and_effects(web):
    r,srv,token,_=ready(web)
    srv.ui.render_delay=60
    field='art.seasons[0]'
    values={field+'.name':' Winter ',field+'.start':'11-01',field+'.end':'02-29',field+'.terms':' snow, trees '}
    assert r('/settings/field', {'csrf':token,'field':field,**values})[0]==200
    from ventanal.config import load_config
    season=load_config(srv.ui.config_path).seasons[0]
    assert season.name=='Winter' and season.terms==('snow','trees')
    assert srv.ui.render_effects=={'pool'}
    assert srv.ui.settings_effects(['schedule.quiet_end','schedule.art_slots'])=={'schedule'}
    assert srv.ui.settings_effects(['display.units','weather.here_label'])=={'weather'}
    assert srv.ui.settings_effects(['art.museums','art.screen_fit_only'])=={'pool'}
    before=srv.ui.config_path.read_bytes()
    assert r('/settings/field', {'csrf':token,'field':field,**{**values,field+'.start':'02-30'}})[0]==400
    assert before==srv.ui.config_path.read_bytes()

def test_fallback_month_day(tmp_path):
    p=tmp_path/'settings.toml';p.write_text(EXAMPLE.read_text())
    cfg=save_settings(p,{'art.seasons[0].end.month':'02','art.seasons[0].end.day':'29'})
    assert cfg.seasons[0].end==(2,29)

def test_worker_pool_only_and_weather_flags(tmp_path,monkeypatch):
    from types import SimpleNamespace
    from ventanal import settings_worker as worker
    calls=[]
    cfg=SimpleNamespace(cache_dir=tmp_path,timezone='UTC')
    monkeypatch.setattr(worker,'load_config',lambda p:cfg)
    monkeypatch.setattr(worker,'ArtPool',lambda p:'pool')
    monkeypatch.setattr(worker,'refresh_pool',lambda *a:calls.append('pool'))
    monkeypatch.setattr(worker,'_tick_locked',lambda *a:calls.append(a[3:]) or {'weather':True})
    monkeypatch.setattr(worker.sys,'argv',['worker','fixture.toml','--refresh-pool'])
    worker.main(); assert calls==['pool']
    calls.clear()
    monkeypatch.setattr(worker.sys,'argv',['worker','fixture.toml','--force-weather'])
    worker.main(); assert calls==[(False,False,True)]
    calls.clear()
    monkeypatch.setattr(worker,'_tick_locked',lambda *a:{'weather':False})
    with pytest.raises(RuntimeError,match='incomplete'): worker.main()

def test_coordinates_can_be_set_together(web):
    r,srv,token,body=ready(web)
    srv.ui.render_delay=60
    assert 'data-field="location.coordinates"' in body
    assert re.search(r'name="location.latitude"[^>]*step="any"', body)
    assert r('/settings/field', {'csrf':token,'field':'location.coordinates','location.latitude':' 47.5 ','location.longitude':' -122.2 '})[0]==200
    from ventanal.config import load_config
    cfg=load_config(srv.ui.config_path)
    assert cfg.latitude==47.5 and cfg.longitude==-122.2

def test_sleep_metadata_without_render(tmp_path,monkeypatch):
    from types import SimpleNamespace
    from ventanal import settings_worker as worker
    from ventanal.manifest import read_manifest
    cfg=SimpleNamespace(cache_dir=tmp_path,output_dir=tmp_path/'out',timezone='UTC',quiet_start='22:00',quiet_end='06:00')
    monkeypatch.setattr(worker,'load_config',lambda p:cfg)
    monkeypatch.setattr(worker.sys,'argv',['worker','fixture.toml','--force-schedule'])
    def forbidden(*args): pytest.fail('A sleep change must not render frames')
    monkeypatch.setattr(worker,'_tick_locked',forbidden)
    worker.main()
    assert read_manifest(cfg.output_dir)['quiet']=={'start':'22:00','end':'06:00'}
