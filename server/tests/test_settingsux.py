# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Stephen de la Vega
import json
import re
import time
import pytest
from test_gui import web, EXAMPLE
from ventanal.settings import save_settings

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
    r,srv,_=web; srv.ui.auth.set_password('secret','secret')
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
    values={field+'.name':' Winter ',field+'.start':'12-01',field+'.end':'02-28',field+'.terms':' snow, trees '}
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
    cfg=save_settings(p,{'art.seasons[0].start.month':'02','art.seasons[0].start.day':'28'})
    assert cfg.seasons[0].start==(2,28)

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
