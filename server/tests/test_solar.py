# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Stephen de la Vega
from datetime import datetime, timezone, timedelta
from io import BytesIO
from zipfile import ZipFile
import numpy as np
import pytest
from PIL import Image
from ventanal.solar.licence import build_index, verify, normalize_call
from ventanal.solar.conditions import conditions, grid_square
from ventanal.solar.data import Solar, xray_class
from ventanal.solar.render import render_solar, SOLAR_READOUT
from ventanal.solar.publish import meaningful, due
from ventanal.quantize import Method, quantize, simulate

NOW = datetime(2026, 11, 17, 4, tzinfo=timezone.utc)

def fixture_zip():
    b = BytesIO()
    with ZipFile(b, 'w') as z:
        z.writestr('HD.dat', 'HD|1|||W1AW|A|HA|01/01/2020|01/01/2030|\nHD|2|||K1OLD|A|HA||01/01/2020|\nHD|3|||K1OFF|C|HA||01/01/2030|\n')
        z.writestr('EN.dat', '\n'.join('|'.join(['EN', n, '', '', call, 'L'] + ['']*12 + [zip5]) for n, call, zip5 in [('1','W1AW','06111-1234'),('2','K1OLD','06111'),('3','K1OFF','06111')]))
        z.writestr('AM.dat', 'AM|1|||W1AW|E|\nAM|2|||K1OLD|G|\nAM|3|||K1OFF|T|\n')
    b.seek(0)
    return b

def test_fcc_gate(tmp_path):
    db = tmp_path/'fcc.sqlite3'
    assert 'not yet downloaded' in verify(db, 'W1AW', '06111', NOW).reason
    build_index(fixture_zip(), db, NOW)
    assert verify(db, 'W1AW', '06111-5555', NOW).ok
    assert 'Extra' in verify(db, 'W1AW', '06111', NOW).reason
    for call, zip5, reason in [('W1AW','99999','ZIP mismatch'),('K1OLD','06111','expired'),('K1OFF','06111','inactive'),('K1NEW','06111','not found')]:
        result = verify(db, call, zip5, NOW)
        assert not result.ok and reason in result.reason
    assert normalize_call(' w1aw ') == 'W1AW'
    with pytest.raises(ValueError): normalize_call('W1AW/../x')
    with pytest.raises(Exception): build_index(BytesIO(b'bad'), db, NOW)
    assert verify(db, 'W1AW', '06111', NOW).ok

def test_estimates():
    assert grid_square(41.7,-72.7) == 'FN31'
    assert grid_square(90,180) == 'RR99'
    quiet = conditions(142, 2, 8, 'B6.3')
    storm = conditions(142, 8, 150, 'X2.0')
    assert quiet.muf == 24 and quiet.noise == 'S1–S2'
    assert all(day == 'Poor' for _, day, _ in storm.bands)
    assert quiet.bands[0][2] == 'Good'
    assert xray_class(6.3e-7) == 'B6.3'

def sample():
    scales={'0':{key:{'Scale':'0'} for key in ('R','S','G')}}
    for i,k in enumerate((3,5,4),1):
        scales[str(i)]={'DateStamp':f'2026-11-{16+i}', 'R':{'MinorProb':'25'},'S':{'Prob':'5'},'G':{'Scale':'1' if k>=5 else '0'}}
    kp=[((NOW-timedelta(hours=21-i*3)).isoformat(),k) for i,k in enumerate((2,2.7,1.9,3.4,4.5,5,3,2))]
    return Solar(142,118,'B6.3',2,8,kp,
                 '00-03UT 3.00 5.00 (G1) 4.00\n', ['WATCH: G1 GEOMAGNETIC STORM'], scales, Image.new('RGB',(540,540),'orange'), '2026-11-17')

def test_publish_policy():
    s = sample(); key = meaningful(s)
    previous = {'key':key,'alerts':s.alerts,'published':NOW.isoformat()}
    assert not due(previous,s,NOW+timedelta(hours=4))
    s.sfi=150
    assert not due(previous,s,NOW+timedelta(hours=2))
    assert due(previous,s,NOW+timedelta(hours=3))
    s.alerts=[]
    assert due(previous,s,NOW+timedelta(minutes=1))

def test_render():
    from ventanal.config import load_config
    from pathlib import Path
    cfg=load_config(Path(__file__).parents[1]/"ventanal.example.toml")
    canvas = render_solar(sample(), 'CALL SIGN', (41.7,-72.7), NOW, cfg)
    r=SOLAR_READOUT
    assert not np.any(np.asarray(canvas.method)[r.y:r.y+r.h,r.x:r.x+r.w] == Method.SOLID)
    assert np.any(np.asarray(canvas.method)[200:740,960:1500] == Method.DIFFUSE)
    preview=simulate(quantize(canvas,cfg.panel_rgb),cfg.panel_rgb)
    golden=Image.open(__file__.replace('test_solar.py','golden/solar.png')).convert('RGB')
    assert np.array_equal(np.asarray(preview),np.asarray(golden))

from test_gui import web
from test_battcal import login
from ventanal.config import load_config
from ventanal.status import parse_board


def test_settings_and_status(web, monkeypatch):
    r,srv,token=login(web)
    srv.ui.render_delay=60
    assert 'Ham radio' in r('/settings')[1]
    assert r('/ham/refresh')[0] == 405
    assert r('/ham/refresh',{'csrf':'bad'})[0] == 403
    data={'csrf':token,'field':'ham.call_sign','ham.call_sign':' w1aw '}
    assert r('/settings/field',data)[0] == 200
    assert load_config(srv.ui.config_path).ham_call_sign == 'W1AW'
    before=srv.ui.config_path.read_bytes()
    assert r('/settings/field',{**data,'ham.call_sign':'bad call'})[0] == 400
    assert srv.ui.config_path.read_bytes() == before
    import json
    state=json.loads(r('/settings/state')[1])
    assert 'not yet downloaded' in state['ham']['reason']
    assert parse_board({'screen':['solar']}) == {'screen':'solar'}
    assert 'SOLAR' in r('/')[1]
    from threading import Event
    started=Event(); release=Event(); calls=[]
    def run(*a,**kw):
        calls.append(1); started.set(); release.wait(2)
        return type('Result',(),{'returncode':0})()
    monkeypatch.setattr('ventanal.ui.subprocess.run',run)
    assert r('/ham/refresh',{'csrf':token})[0]==200
    assert started.wait(1)
    assert r('/ham/refresh',{'csrf':token})[0]==200
    assert len(calls)==1
    release.set()


def test_data_schema_and_cache(tmp_path):
    import httpx
    from ventanal.solar.data import fetch_solar
    count=[]
    image=BytesIO(); Image.new('RGB',(16,16),'yellow').save(image,format='JPEG')
    def respond(request):
        count.append(str(request.url))
        assert request.headers['User-Agent'] == 'Ventanal/1.0.0 (+https://github.com/sdelavega/ventanal)'
        assert 'W1AW' not in str(request.url)
        path=request.url.path
        if path.endswith('.jpg'): return httpx.Response(200,content=image.getvalue())
        if 'daily-solar' in path: return httpx.Response(200,text='2026 11 16 142 118\n')
        if '3-day-forecast' in path: return httpx.Response(200,text='00-03UT 2.00 4.67 (G1) 2.33\n')
        if 'planetary-k' in path: value=[{'time_tag':'2026-11-17T00:00:00','Kp':2,'a_running':8}]
        elif 'xrays' in path: value=[{'time_tag':'2026-11-17T04:00:00','energy':'0.1-0.8nm','flux':6.3e-7}]
        elif 'scales' in path: value={'1':{'DateStamp':'2026-11-17','R':{'MinorProb':'25'},'S':{'Prob':'5'},'G':{'Scale':'1'}}}
        else: value=[{'product_id':'A','issue_datetime':'2026-11-17 01:00:00','message':'WATCH: G1 storm\nValid To: 2026 Nov 18 0000 UTC'}, {'product_id':'B','issue_datetime':'2026-11-16 01:00:00','message':'WARNING: old\nValid To: 2026 Nov 16 0000 UTC'}]
        return httpx.Response(200,json=value)
    with httpx.Client(transport=httpx.MockTransport(respond),headers={'User-Agent':'wrong-client-default'}) as client:
        s=fetch_solar(client,tmp_path,NOW)
        assert (s.sfi,s.ssn,s.xray,s.k,s.a)==(142,118,'B6.3',2,8)
        assert s.alerts==['WATCH: G1 storm · UNTIL Nov 18 0000 UTC']
        fetch_solar(client,tmp_path,NOW+timedelta(seconds=30))
        assert len(count)==7


def test_publication_gate_and_signed_solar(tmp_path,monkeypatch):
    from dataclasses import replace
    from pathlib import Path
    from ventanal.solar.publish import tick_solar
    from ventanal.manifest import read_manifest
    from ventanal.security import Security, canonical
    import hmac,hashlib
    cfg=replace(load_config(Path(__file__).parents[1]/'ventanal.example.toml'),ham_call_sign='W1AW',zip='06111',latitude=41.7,longitude=-72.7,cache_dir=tmp_path/'cache',output_dir=tmp_path/'out')
    build_index(fixture_zip(),cfg.cache_dir/'fcc.sqlite3',NOW)
    monkeypatch.setattr('ventanal.solar.publish.fetch_solar',lambda *a:sample())
    import ventanal.solar.publish as solar_publish
    headers=[]
    real_render=solar_publish.render_solar
    def capture(s,call,*args):
        headers.append(call)
        return real_render(s,call,*args)
    monkeypatch.setattr(solar_publish,'render_solar',capture)
    assert tick_solar(cfg,NOW,None)
    assert headers == [cfg.ham_call_sign]
    manifest=read_manifest(cfg.output_dir)
    assert 'solar' in manifest['screens']
    security=Security(cfg.cache_dir)
    security.enable(["localhost"])
    signed=security.manifest(manifest,cfg.output_dir.resolve())
    sig=signed.pop('sig')
    assert hmac.compare_digest(sig,hmac.new(security.key,canonical(signed),hashlib.sha256).hexdigest())
    assert len(signed['screens']['solar']['sha256'])==64
    assert not tick_solar(cfg,NOW+timedelta(minutes=1),None)
    assert not tick_solar(replace(cfg,zip='99999'),NOW+timedelta(minutes=2),None)
    assert 'solar' not in read_manifest(cfg.output_dir)['screens']
    assert len(headers) == 1
    assert not tick_solar(cfg,NOW+timedelta(minutes=3),None)
    assert tick_solar(cfg,NOW+timedelta(hours=3),None)


def test_noaa_watch_and_extended_warning_formats():
    from ventanal.solar.data import active_alerts
    rows=[{'product_id':'WATCH','issue_datetime':'2026-11-16 00:00:00','message':'WATCH: Geomagnetic Storm Category G1 Predicted\nHighest Storm Level Predicted by Day:\nNov 16: None (Below G1) Nov 17: G1 (Minor) Nov 18: G1 (Minor)\n'},
          {'product_id':'WARN','issue_datetime':'2026-11-17 00:00:00','message':'EXTENDED WARNING: K-index 5\nNow Valid Until: 2026 Nov 17 0900 UTC'}]
    active=active_alerts(rows,NOW)
    assert len(active)==2 and any('Nov 18' in s for s in active)
    assert not active_alerts(rows,NOW+timedelta(days=3))


def test_fcc_weekly_refresh_and_failure_cleanup(tmp_path):
    import httpx
    from ventanal.solar.licence import refresh,FCC_URL
    requests=[]; fail=False
    def respond(request):
        requests.append(str(request.url))
        assert request.headers['User-Agent'] == 'Ventanal/1.0.0 (+https://github.com/sdelavega/ventanal)'
        return httpx.Response(503) if fail else httpx.Response(200,content=fixture_zip().getvalue())
    with httpx.Client(transport=httpx.MockTransport(respond),headers={'User-Agent':'wrong-client-default'}) as client:
        assert refresh(tmp_path,client,NOW)
        assert not refresh(tmp_path,client,NOW+timedelta(days=6))
        assert refresh(tmp_path,client,NOW+timedelta(days=7))
        assert requests==[FCC_URL,FCC_URL]
        before=(tmp_path/'fcc.sqlite3').read_bytes()
        fail=True
        with pytest.raises(httpx.HTTPStatusError): refresh(tmp_path,client,NOW,force=True)
        assert (tmp_path/'fcc.sqlite3').read_bytes()==before
        assert not (tmp_path/'fcc-download.zip').exists()


def test_worker_error_does_not_echo_sensitive_message(tmp_path,monkeypatch,capsys):
    from ventanal.solar import worker
    from dataclasses import replace
    from pathlib import Path
    cfg=replace(load_config(Path(__file__).parents[1]/'ventanal.example.toml'),cache_dir=tmp_path)
    monkeypatch.setattr(worker,'load_config',lambda *a:cfg)
    monkeypatch.setattr(worker.sys,'argv',['worker','fixture.toml'])
    def fail(*a,**kw): raise PermissionError(30,'private error contents')
    monkeypatch.setattr(worker,'refresh',fail)
    assert worker.main()==1
    error=capsys.readouterr().err
    assert 'PermissionError' in error and 'private error contents' not in error


def test_corrupt_database_requests_refresh(tmp_path):
    db = tmp_path/'fcc.sqlite3'
    db.write_bytes(b'corrupt')
    assert 'refresh' in verify(db, '', None, NOW).reason.lower()


@pytest.mark.parametrize('previous', [{'key':'old'}, {'alerts':sample().alerts}, [], {'published':'bad','alerts':sample().alerts}])
def test_partial_publication_state_heals(previous):
    assert due(previous, sample(), NOW)


def test_fcc_contention_is_already_running(tmp_path, monkeypatch, capsys):
    import fcntl
    from dataclasses import replace
    from pathlib import Path
    from ventanal.solar import worker
    cfg=replace(load_config(Path(__file__).parents[1]/'ventanal.example.toml'), cache_dir=tmp_path)
    monkeypatch.setattr(worker,'load_config',lambda *a:cfg)
    monkeypatch.setattr(worker.sys,'argv',['worker','fixture.toml'])
    with (tmp_path/'fcc.lock').open('a') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        assert worker.main() == 2
    assert 'already running' in capsys.readouterr().out.lower()


def test_refresh_does_not_hold_tick_lock(tmp_path, monkeypatch):
    import fcntl
    from dataclasses import replace
    from pathlib import Path
    from ventanal import jobs
    import ventanal.solar.licence as licence
    cfg=replace(load_config(Path(__file__).parents[1]/'ventanal.example.toml'), cache_dir=tmp_path, ham_call_sign='W1AW')
    events=[]
    def refresh(*args, **kwargs):
        with (tmp_path/'tick.lock').open('a') as lock:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        events.append('refresh')
    monkeypatch.setattr(licence,'refresh',refresh)
    monkeypatch.setattr(jobs,'_tick_locked',lambda *a: events.append('tick'))
    jobs.tick(cfg,NOW,None)
    assert events == ['refresh','tick']


def test_preview_rechecks_solar_gate(web):
    import sqlite3
    from ventanal.manifest import publish
    r,srv,token=login(web)
    srv.ui.render_delay=60
    assert r('/settings/field',{'csrf':token,'field':'ham.call_sign','ham.call_sign':'W1AW'})[0] == 200
    cfg=load_config(srv.ui.config_path)
    db=cfg.cache_dir/'fcc.sqlite3'
    build_index(fixture_zip(),db,NOW)
    with sqlite3.connect(db) as conn:
        conn.execute('update licences set zip=? where call=?',(cfg.zip[:5],'W1AW'))
    from ventanal.frame import encode_frame
    entry=publish(cfg.output_dir,'solar',encode_frame(np.zeros((1200,1600),dtype=np.uint8)),SOLAR_READOUT,None,Image.new('RGB',(16,16)),NOW)
    ident=entry['id']
    assert r(f'/ui-preview/solar/{ident}.png')[0] == 200
    with sqlite3.connect(db) as conn:
        conn.execute("update licences set status='C'")
    assert r(f'/ui-preview/solar/{ident}.png')[0] == 404


def test_solar_display_conventions(monkeypatch):
    from pathlib import Path
    from ventanal.solar import render
    from ventanal.palette import PANEL_INDEX
    cfg=load_config(Path(__file__).parents[1]/'ventanal.example.toml')
    labels=[]
    original=render._text
    def capture(canvas,text,xy,px,colour,**kw):
        labels.append((text,colour))
        return original(canvas,text,xy,px,colour,**kw)
    monkeypatch.setattr(render,'_text',capture)
    s=sample(); s.k=4.67; s.forecast='00-03UT 4.67 4.33 3.00'
    render.render_solar(s,'CALL SIGN',(41.7,-72.7),NOW,cfg)
    texts=[row[0] for row in labels]
    assert ('5',cfg.panel_rgb[PANEL_INDEX['red']]) in labels
    assert any('KP MAX 5-' in value for value in texts)
    assert any('KP MAX 4+' in value for value in texts)
    assert any('TUE 17' == value for value in texts)
    assert any('QUIET' in value for value in texts)
    assert any('NOAA SCALES' in value and '(NOW)' in value for value in texts)
    assert ('POOR',cfg.panel_rgb[PANEL_INDEX['white']]) in labels
    assert not any('2026-11-' in value for value in texts)


def test_noaa_summary_preserves_type_scale_dates():
    from ventanal.solar.render import alert_summary, kp_notation, crop_sun
    assert [kp_notation(k) for k in (0,3,4.33,4.67,9)] == ['0','3','4+','5-','9']
    label,message=alert_summary(['WATCH: Geomagnetic Storm Category G1 Predicted · Nov 17 G1, Nov 18 G1'])
    assert label == 'WATCH' and 'G1' in message and 'NOV 17' in message and 'NOV 18' in message
    sun=Image.new('RGB',(1024,1024),'black')
    from PIL import ImageDraw
    ImageDraw.Draw(sun).rectangle((0,980,1023,1023),fill='white')
    assert np.asarray(crop_sun(sun)).max() == 0


@pytest.mark.parametrize('k,expected',[(3,'green'),(4,'yellow'),(5,'red'),(0.67,'green')])
def test_current_integer_k_colour(k,expected):
    from ventanal.solar.render import integer_k,k_color
    assert k_color(integer_k(k)) == expected


def test_chart_full_scale_and_minimum_zero_bar():
    from pathlib import Path
    from ventanal.solar.render import LAYOUT
    from ventanal.palette import PANEL_INDEX
    cfg=load_config(Path(__file__).parents[1]/'ventanal.example.toml')
    s=sample(); s.kp=[('2026-11-17T00:00:00',9),('2026-11-17T03:00:00',0)]
    canvas=render_solar(s,'CALL SIGN',(41.7,-72.7),NOW,cfg)
    x,y,w,h=LAYOUT['chart']
    rgb=np.asarray(canvas.rgb)
    assert tuple(rgb[y,x+20]) == cfg.panel_rgb[PANEL_INDEX['red']]
    assert tuple(rgb[y+h-4,int(x+w/8+20)]) == cfg.panel_rgb[PANEL_INDEX['green']]
    assert tuple(rgb[y+h-5,int(x+w/8+20)]) != cfg.panel_rgb[PANEL_INDEX['green']]


def test_settings_contention_message(web,monkeypatch):
    import time
    r,srv,token=login(web)
    monkeypatch.setattr('ventanal.ui.subprocess.run',lambda *a,**kw:type('Result',(),{'returncode':2})())
    assert r('/ham/refresh',{'csrf':token})[0] == 200
    for _ in range(50):
        if not srv.ui.fcc_refreshing: break
        time.sleep(.01)
    assert srv.ui.fcc_result == 'Database refresh already running'


@pytest.mark.parametrize('storm', [False, True])
def test_solar_layout_clearance(monkeypatch, storm):
    """Actual fitted glyph bounds clear other text and the shared frame."""
    from pathlib import Path
    from ventanal.solar import render
    from ventanal.weather.lcars import draw_frame
    from ventanal.quantize import Canvas
    from ventanal.text import font, fit, renderable
    cfg=load_config(Path(__file__).parents[1]/'ventanal.example.toml')
    frame=Canvas()
    draw_frame(frame, [(y,h,color) for y,h,color,_ in render.LAYOUT['sidebar']])
    occupied=np.any(np.asarray(frame.rgb)!=0,axis=2)
    boxes=[]
    original=render._text
    def capture(canvas,text,xy,px,colour,**kw):
        face_name='antonio-bold' if kw.get('bold') else 'antonio'
        face=font(face_name,px)
        label=renderable(text,face_name)
        if kw.get('max_w') is not None: label=fit(label,face,kw['max_w'])
        l,t,r,b=face.getbbox(label,anchor=kw.get('anchor','ls'))
        box=(round(xy[0]+l),round(xy[1]+t),round(xy[0]+r),round(xy[1]+b))
        if xy[0]>260:
            x0,y0,x1,y1=box
            assert 330<=x0<x1<=1570 and 76<=y0<y1<=1124, (label,box)
            assert not occupied[y0:y1,x0:x1].any(), (label,box,'frame')
            for other,prev in boxes:
                assert min(x1,prev[2])<=max(x0,prev[0]) or min(y1,prev[3])<=max(y0,prev[1]), (label,other,box,prev)
            sx,sy,size=render.LAYOUT['sun']
            assert min(x1,sx+size)<=max(x0,sx) or min(y1,sy+size)<=max(y0,sy), (label,box,'sun')
            boxes.append((label,box))
        return original(canvas,text,xy,px,colour,**kw)
    monkeypatch.setattr(render,'_text',capture)
    s=sample()
    if storm:
        s.k=9; s.a=150; s.xray='X12.3'; s.sfi=299; s.ssn=299
        s.alerts=['WARNING: RADIO BLACKOUT R3 UNTIL NOV 19 2026 UTC', 'WATCH: GEOMAGNETIC STORM G5 NOV 20 2026 UTC']
    render.render_solar(s,'CALL SIGN',(41.7,-72.7),NOW,cfg)
    bounds=dict(boxes)
    bounds[f'{s.sfi:.0f}']=next(b for t,b in boxes if t==f'{s.sfi:.0f}' and b[0]==330)
    assert 1124-max(b[3] for t,b in boxes if t in ('ALERT','WATCH') or 'GEOMAGNETIC STORM' in t or 'RADIO BLACKOUT' in t)>=24
    forecast=[b for t,b in boxes if t in ('TUE 17','WED 18','THU 19') or t.startswith(('KP MAX','R1'))]
    alert=[b for t,b in boxes if t in ('ALERT','WATCH') or 'GEOMAGNETIC STORM' in t or 'RADIO BLACKOUT' in t]
    assert min(b[1] for b in alert)-max(b[3] for b in forecast)>=20
    for label,value in [('SOLAR FLUX (SFI)',f'{s.sfi:.0f}'),('X-RAY',s.xray)]:
        gap=bounds[value][1]-bounds[label][3]
        assert 4<=gap<=16, (label,gap)
    assert abs((bounds[f'{s.sfi:.0f}'][1]-bounds['SOLAR FLUX (SFI)'][3])-(bounds[s.xray][1]-bounds['X-RAY'][3]))<=1
    # Content-row clearance uses visible glyphs, not just nominal font sizes.
    metric_bottom=max(b[3] for t,b in boxes if b[1]<450 and t in (str(s.ssn),s.xray,str(render.integer_k(s.k)),str(s.a)))
    summary=next(b for t,b in boxes if t.startswith('FIELD '))
    assert summary[1]-metric_bottom>=28
    assert bounds['DAY'][1]-summary[3]>=28
    assert render.LAYOUT['bands'][2]-38>=28
    assert render.LAYOUT['chart'][1]-bounds['HF CONDITIONS, MUF AND NOISE ARE ESTIMATES'][3]>=28
    assert min(b[1] for b in forecast)-bounds['PLANETARY KP · 3-HOUR INTERVALS · UTC'][3]>=28
    fy,fh=render.LAYOUT['sidebar'][-1][:2]
    assert fy<=min(b[1] for b in forecast) and fy+fh>=max(b[3] for b in forecast)
    rows=render.LAYOUT['sidebar']
    assert {rows[i+1][0]-(y+h) for i,(y,h,_,_) in enumerate(rows[:-1])}=={6}


@pytest.mark.parametrize('rating', [
    'GOOD',
    'FAIR', 'POOR',
])
def test_band_simulated_panel_contrast(monkeypatch, rating):
    from dataclasses import replace
    from pathlib import Path
    from ventanal.solar import render
    from ventanal.palette import PANEL_INDEX, to_oklab
    cfg=load_config(Path(__file__).parents[1]/'ventanal.example.toml')
    cfg=replace(cfg,panel_rgb=tuple(tuple(bytes.fromhex(h)) for h in ('2a272e','c1c5b3','d6c547','652922','3c5b91','567359')))
    original=render._text
    seen=set()
    def capture(canvas,text,xy,px,colour,**kw):
        if text == rating:
            fill={'GOOD':'green','FAIR':'yellow','POOR':'red'}[text]
            expected='black' if text=='FAIR' else 'white'
            assert colour==cfg.panel_rgb[PANEL_INDEX[expected]]
            delta=abs(to_oklab(np.array(colour))[0]-to_oklab(np.array(cfg.panel_rgb[PANEL_INDEX[fill]]))[0])
            assert delta >= (.28 if text == 'GOOD' else .35), (text,delta)
            seen.add(text)
        return original(canvas,text,xy,px,colour,**kw)
    monkeypatch.setattr(render,'_text',capture)
    for k in (2,9):
        s=sample(); s.k=k
        render.render_solar(s,'CALL SIGN',(41.7,-72.7),NOW,cfg)
    assert seen=={rating}


def test_band_text_uses_requested_solid_inks(monkeypatch):
    from pathlib import Path
    from ventanal.solar import render
    from ventanal.palette import PANEL_INDEX
    cfg=load_config(Path(__file__).parents[1]/'ventanal.example.toml')
    original=render._text
    seen=set()
    def capture(canvas,text,xy,px,colour,**kw):
        if text in ('GOOD','FAIR','POOR'):
            assert colour==cfg.panel_rgb[PANEL_INDEX['black' if text=='FAIR' else 'white']]
            seen.add(text)
        return original(canvas,text,xy,px,colour,**kw)
    monkeypatch.setattr(render,'_text',capture)
    for k in (2,9):
        s=sample(); s.k=k
        render.render_solar(s,'CALL SIGN',(41.7,-72.7),NOW,cfg)
    assert seen=={'GOOD','FAIR','POOR'}


@pytest.mark.parametrize('storm', [False, True])
def test_solar_round_four_rows_and_type(monkeypatch, storm):
    from pathlib import Path
    from ventanal.solar import render
    cfg=load_config(Path(__file__).parents[1]/'ventanal.example.toml')
    original=render._text
    calls=[]
    def capture(canvas,text,xy,px,colour,**kw):
        calls.append((text,xy,px,kw))
        assert px >= 18, (text,px)
        if text in ('GOOD','FAIR','POOR'):
            assert px == 22 and kw.get('bold'), (text,px,kw)
        return original(canvas,text,xy,px,colour,**kw)
    monkeypatch.setattr(render,'_text',capture)
    s=sample()
    if storm:
        s.k=9; s.a=150; s.xray='X12.3'; s.sfi=299; s.ssn=299
    render.render_solar(s,'CALL SIGN',(41.7,-72.7),NOW,cfg)
    def at(label): return next(c for c in calls if c[0]==label)
    assert at(f'{s.sfi:.0f}')[2] == 120
    for value in (str(s.ssn),s.xray,str(render.integer_k(s.k)),str(s.a)):
        assert any(t==value and px==56 for t,xy,px,kw in calls), value
    assert any('NOAA SWPC · NASA SDO' in t and xy==(332,194) and px==24 for t,xy,px,_ in calls)
    assert at('SOLAR CONDITIONS')[1:3] == ((330,148),52)
    assert at('DAY')[1][1] >= next(xy[1] for t,xy,_,_ in calls if t.startswith('FIELD '))+28
    assert at('HF CONDITIONS, MUF AND NOISE ARE ESTIMATES')[1][1] > render.LAYOUT['bands'][1]+3*render.LAYOUT['bands'][2]+38
    x,y,w,h=render.LAYOUT['chart']
    assert w>=1170 and h>=90
    assert at('PLANETARY KP · 3-HOUR INTERVALS · UTC')[1][1] > y+h+28
    assert any(t=='G1 (KP 5)' for t,_,_,_ in calls)
    assert render.LAYOUT['sun'][2]==340
    assert render.LAYOUT['bands'][2]>=56
    for t,xy,px,kw in calls:
        if t in ('TUE 17','WED 18','THU 19'): assert px==32
        elif t.startswith('KP MAX'): assert px==24
        elif t.startswith('R1'): assert px==20
        elif t.startswith('FIELD '): assert px==22
        elif t in ('SUNSPOTS','X-RAY','K-INDEX','A-INDEX'): assert px==20
    assert sum(t in ('TUE 17','WED 18','THU 19') for t,_,_,_ in calls)==3


def test_kp_storm_bars_preserve_g1_reference():
    from pathlib import Path
    from ventanal.solar.render import LAYOUT
    from ventanal.palette import PANEL_INDEX
    cfg=load_config(Path(__file__).parents[1]/'ventanal.example.toml')
    s=sample(); s.kp=[(stamp,9) for stamp,_ in s.kp]
    canvas=render_solar(s,'CALL SIGN',(41.7,-72.7),NOW,cfg)
    x,y,w,h=LAYOUT['chart']
    assert tuple(np.asarray(canvas.rgb)[int(y+h-5/9*h),x+20]) == cfg.panel_rgb[PANEL_INDEX['yellow']]


@pytest.mark.parametrize('scale', ['0', '5'])
def test_solar_captions_centred_on_sun(monkeypatch, scale):
    from pathlib import Path
    from ventanal.solar import render
    from ventanal.text import font
    cfg=load_config(Path(__file__).parents[1]/'ventanal.example.toml')
    original=render._text
    captions=[]
    def capture(canvas,text,xy,px,colour,**kw):
        if text.startswith(('SUN TODAY', 'NOAA SCALES')):
            l,_,r,_=font('antonio',px).getbbox(text,anchor=kw.get('anchor','ls'))
            captions.append(xy[0]+(l+r)/2)
        return original(canvas,text,xy,px,colour,**kw)
    monkeypatch.setattr(render,'_text',capture)
    s=sample()
    s.scales['0']={key:{'Scale':scale} for key in ('R','S','G')}
    render.render_solar(s,'CALL SIGN',(41.7,-72.7),NOW,cfg)
    sx,_,diameter=render.LAYOUT['sun']
    assert len(captions)==2
    assert all(abs(centre-(sx+diameter/2))<=1 for centre in captions), captions


def test_solar_warm_frame_preserves_geometry_pattern_and_readout():
    from pathlib import Path
    from PIL import ImageColor
    from ventanal.palette import LCARS_MODERN
    from ventanal.quantize import Canvas
    from ventanal.weather.lcars import draw_frame
    from ventanal.solar.render import LAYOUT
    cfg=load_config(Path(__file__).parents[1]/'ventanal.example.toml')
    solar=render_solar(sample(),'CALL SIGN',(41.7,-72.7),NOW,cfg)
    reference=Canvas()
    segments=[(y,h,colour) for y,h,colour,_ in LAYOUT['sidebar']]
    draw_frame(reference,segments)
    baseline=np.asarray(reference.rgb)
    rgb=np.asarray(solar.rgb)
    frame=np.all(baseline==ImageColor.getrgb(LCARS_MODERN['violet']),axis=2)
    # Violet is now also a sidebar fill; isolate the elbows from the sidebar.
    for y, h, _, _ in LAYOUT['sidebar']:
        frame[y:y+h, 30:260] = False
    assert frame.any()
    assert np.all(rgb[frame]==ImageColor.getrgb(LCARS_MODERN['orange']))
    assert np.all(np.asarray(solar.method)[frame]==Method.PATTERN)
    warm=Canvas()
    draw_frame(warm,segments,accent=LCARS_MODERN['orange'])
    assert np.array_equal(np.any(baseline!=0,axis=2),np.any(np.asarray(warm.rgb)!=0,axis=2))
    assert np.array_equal(np.asarray(reference.method),np.asarray(warm.method))
    r=SOLAR_READOUT
    assert np.array_equal(rgb[r.y:r.y+r.h,r.x:r.x+r.w],baseline[r.y:r.y+r.h,r.x:r.x+r.w])
    assert [colour for _,_,colour,_ in LAYOUT['sidebar']]==['sky','red','gold','sky','violet']


@pytest.mark.parametrize('screen', ['solar', 'weather'])
@pytest.mark.parametrize('calibrated', [False, True])
def test_sidebar_ink_maximises_simulated_panel_lightness(monkeypatch, screen, calibrated):
    from dataclasses import replace
    from pathlib import Path
    from PIL import ImageColor
    from ventanal.palette import LCARS_MODERN, to_oklab
    from ventanal.quantize import Canvas, quantize, simulate
    from ventanal.solar import render as solar
    from ventanal.weather import render as weather
    from test_weather_render import sample as weather_sample
    cfg=load_config(Path(__file__).parents[1]/'ventanal.example.toml')
    if calibrated:
        cfg=replace(cfg, panel_rgb=tuple(tuple(bytes.fromhex(h)) for h in
                    ('2a272e','c1c5b3','d6c547','652922','3c5b91','567359')))
    module=solar if screen=='solar' else weather
    original=module._text
    inks={}
    def capture(canvas, label, xy, px, colour, **kw):
        if xy[0]==248:
            inks[xy[1]]=colour
        return original(canvas,label,xy,px,colour,**kw)
    monkeypatch.setattr(module,'_text',capture)
    if screen=='solar':
        solar.render_solar(sample(),'CALL SIGN',(41.7,-72.7),NOW,cfg)
        segments=[(y,h,c) for y,h,c,_ in solar.LAYOUT['sidebar']]
    else:
        args=list(weather_sample.__wrapped__()); args[-1]=cfg
        weather.render_weather(*args)
        segments=[(212,118,'sky'),(336,252,'orange'),(594,66,'light_gray'),(666,322,'gray')]
    for y,h,colour in segments:
        tile=Canvas((8,8),fill=ImageColor.getrgb(LCARS_MODERN[colour]))
        mean=np.asarray(simulate(quantize(tile,cfg.panel_rgb),cfg.panel_rgb)).mean(axis=(0,1))
        background_l=to_oklab(mean)[0]
        lightness=to_oklab(np.asarray(cfg.panel_rgb)[:2])[:,0]
        expected=cfg.panel_rgb[int(np.argmax(abs(lightness-background_l)))]
        assert inks[y+h-12]==expected
    assert len(inks)==len(segments)
