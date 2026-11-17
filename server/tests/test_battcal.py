# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Stephen de la Vega
from datetime import datetime, timedelta, timezone
import hashlib
import hmac
import json
import pytest
from test_gui import web
from ventanal.config import load_config
from ventanal.security import canonical
from ventanal.status import parse_board


def login(web):
    r, srv, _ = web
    srv.ui.auth.set_password('fixture', 'fixture')
    _, _, token = r('/login')
    assert r('/login', {'csrf': token, 'password': 'fixture'})[0] == 303
    return r, srv, r('/')[2]


def test_telemetry():
    assert parse_board({'bat_mv':['4144']}) == {'bat_mv':4144}
    for value in ['-1', '04144', '4.144', '99999']:
        assert parse_board({'bat_mv':[value]}) == {}
    assert parse_board({'bat_mv':['4144','4145']}) == {}


def test_auth(web):
    r, srv, _ = web
    srv.ui.auth.set_password('fixture','fixture')
    assert r('/battery', {'action':'full'})[0] == 303
    r, srv, token = login(web)
    assert r('/battery', {'csrf':'wrong','action':'full'})[0] == 403
    assert r('/battery')[0] == 405


def test_full_reset_roundtrip(web):
    r, srv, token = login(web)
    srv.ui.store.checkin('127.0.0.1', {'bat':95,'bat_mv':4144})
    srv.ui.render_delay=60
    assert r('/settings/field', {'csrf':token,'browser_session':'batteryfixture','field':'display.units','display.units':'metric'})[0]==200
    before = srv.ui.config_path.read_text()
    assert r('/battery', {'csrf':token, 'action':'full', 'confirm':'yes', 'full_mv':'4299', 'browser_session':'batteryfixture'})[0] == 303
    assert load_config(srv.ui.config_path).battery_full_mv == 4144
    assert '# Quiet hours' in srv.ui.config_path.read_text()
    assert len(list((srv.ui.config_path.parent/'backups').glob('*.toml'))) == 1
    body = r('/')[1]
    assert '4.144 V' in body and 'Calibrated: full = 4.144 V' in body
    assert r('/battery', {'csrf':token,'action':'reset'})[0] == 303
    assert load_config(srv.ui.config_path).battery_full_mv is None
    assert '[battery]' not in before
    assert 'full_mv' not in srv.ui.config_path.read_text()


@pytest.mark.parametrize('mv,age', [(None,0),(3999,0),(4301,0),(4144,7201),(4144,-60)])
def test_reject_report(web,mv,age):
    r, srv, token = login(web)
    if mv is not None:
        srv.ui.store.checkin('127.0.0.1', {'bat_mv':mv})
        at=(datetime.now(timezone.utc)-timedelta(seconds=age)).isoformat()
        with srv.ui.store.connect() as db:
            db.execute('UPDATE board_checkins SET time=?',(at,)); db.commit()
    before=srv.ui.config_path.read_bytes()
    status,body,_=r('/battery', {'csrf':token,'action':'full','confirm':'yes'})
    assert status == 400 and ('report' in body or '4000' in body)
    assert srv.ui.config_path.read_bytes() == before
    assert not list((srv.ui.config_path.parent/'backups').glob('*.toml'))


def test_confirmation_and_password_required(web):
    r,srv,token=login(web)
    srv.ui.store.checkin('127.0.0.1',{'bat_mv':4144})
    status,body,_=r('/battery',{'csrf':token,'action':'full'})
    assert status==200 and 'Only do this after the frame has been charging for several hours' in body
    assert load_config(srv.ui.config_path).battery_full_mv is None
    srv.ui.auth.none()
    token=r('/')[2]
    assert r('/battery',{'csrf':token,'action':'full','confirm':'yes'})[0]==403


@pytest.mark.parametrize('mv',[4000,4300])
def test_bounds_and_signed_manifest(web,mv):
    r,srv,token=login(web)
    srv.ui.store.checkin('127.0.0.1',{'bat_mv':mv})
    assert r('/battery',{'csrf':token,'action':'full','confirm':'yes'})[0]==303
    (srv.ui.config_path.parent/'manifest.json').write_text('{"version":1,"screens":{}}')
    assert json.loads(r('/manifest.json')[1])['battery']=={'full_mv':mv}
    srv.ui.security.enable(['localhost','127.0.0.1'])
    srv.ui.enable_security()
    status,body,_=r('/manifest.json?bat_mv=4144')
    assert status==200
    manifest=json.loads(body)
    sig=manifest.pop('sig')
    assert manifest['battery']=={'full_mv':mv}
    assert hmac.new(srv.ui.security.key,canonical(manifest),hashlib.sha256).hexdigest()==sig
    manifest['battery']['full_mv']=4200 if mv!=4200 else 4144
    assert hmac.new(srv.ui.security.key,canonical(manifest),hashlib.sha256).hexdigest()!=sig
    assert srv.ui.store.latest()['bat_mv']==4144


def test_settings_readonly_and_checkbox_markup(web):
    r,srv,_=web; srv.ui.auth.none()
    body=r('/settings')[1]
    assert 'class="colour-swatch' in body
    assert 'Ignored artworks:' in body
    assert '<input value="#' not in body
    assert 'class="checkbox-row"' in body


def test_voltage_report_proof_tampering(web):
    from http.client import HTTPSConnection
    import ssl
    from test_security import proof
    r,srv,token=login(web)
    (srv.ui.config_path.parent/'manifest.json').write_text('{"version":1,"screens":{}}')
    srv.ui.security.enable(['localhost','127.0.0.1']); srv.ui.enable_security()
    path='/manifest.json?bat_mv=4144'
    def get(path, headers):
        conn=HTTPSConnection('127.0.0.1',srv.tls_server.server_address[1],context=ssl.create_default_context(cadata=srv.ui.security.certificate))
        conn.request('GET',path,headers=headers)
        response=conn.getresponse(); response.read(); conn.close()
        return response.status
    headers=proof(srv.ui.security.key,path,1)
    assert get(path.replace('4144','4200'),headers)==403
    assert srv.ui.store.latest() is None
    assert get(path,headers)==200
    assert srv.ui.store.latest()['bat_mv']==4144
