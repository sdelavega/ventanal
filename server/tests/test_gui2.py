# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Stephen de la Vega
from dataclasses import replace
from datetime import datetime, timezone
from http.client import HTTPConnection
from io import BytesIO
import json
import re
import time

import numpy as np
from PIL import Image
import pytest

from test_gui import web, EXAMPLE
from test_calibrate import cli_config
from test_jobs import setup_job
from ventanal.config import load_config
from ventanal.calibrate import chart
from ventanal.quantize import simulate


def ready(web, path='/calibration'):
    request, server, root = web
    server.ui.auth.none()
    _, body, token = request('/settings')
    request(path)
    return request, server, root, token


def upload(server, token, data, filename='photo.png', content_type='image/png', length=None):
    session, cookie = server.ui.auth.issue(True)
    # Bind the upload and subsequent requests to this explicit session.
    token = session['csrf'] if token is None else token
    boundary = 'ventanal-test-boundary'
    body = (f'--{boundary}\r\nContent-Disposition: form-data; name="photo"; filename="{filename}"\r\nContent-Type: {content_type}\r\n\r\n').encode()+data+f'\r\n--{boundary}--\r\n'.encode()
    c = HTTPConnection(*server.server_address)
    c.request('POST', '/calibration/upload', body, {'Cookie':f'ventanal={cookie}', 'X-CSRF-Token':token, 'Content-Type':f'multipart/form-data; boundary={boundary}', 'Content-Length':str(length or len(body))})
    response=c.getresponse(); result=(response.status,response.read().decode()); c.close()
    return result, session, cookie


def session_request(server, cookie, path, values=None):
    from urllib.parse import urlencode
    c=HTTPConnection(*server.server_address)
    c.request('GET' if values is None else 'POST',path,None if values is None else urlencode(values), {'Cookie':f'ventanal={cookie}','Content-Type':'application/x-www-form-urlencoded'})
    response=c.getresponse(); result=(response.status,response.read()); c.close(); return result


def test_paper_balance_vector_and_validation():
    from ventanal.calibrate import white_balance, paper_mean, map_corners
    raw=['1a1a25','879297','96923b','451c1b','27427a','3a544a']
    expected=['2a272e','c1c5b3','d6c547','652922','3c5b91','567359']
    rgb=lambda values:[tuple(bytes.fromhex(v)) for v in values]
    assert np.max(np.abs(np.array(white_balance(rgb(raw),(171,181,206)))-rgb(expected)))<=1
    assert paper_mean(Image.new('RGB',(20,20),(171,181,206)),(1,2,10,12)) == (171,181,206)
    assert map_corners([(0,0),(1199,0),(1199,899),(0,899)],(1200,900),(4000,3000)) == [(0,0),(3999,0),(3999,2999),(0,2999)]
    for paper in [(0,0,0),(float('nan'),1,2),(-1,2,3)]:
        with pytest.raises(ValueError): white_balance(rgb(raw),paper)
    for rect in [(0,0,0,3),(-1,0,2,2),(19,0,2,2)]:
        with pytest.raises(ValueError): paper_mean(Image.new('RGB',(20,20)),rect)


def test_art_ignore_roundtrip_comments_and_spare(web):
    from ventanal.art.sources import Candidate
    from ventanal.art.pool import ArtPool
    request,server,root,token=ready(web,'/art')
    candidate=Candidate('met','123','A <painting>','Artist','1900','The Met','https://example.test/art',1600,1200)
    pool=ArtPool(root/'cache/art.sqlite3'); pool.add([candidate],'winter')
    server.ui.store.put_spare(1,{'id':'a'*16,'url':'/frames/spare.vnf','credit':{'title':candidate.title}},candidate)
    server.ui.store.register('b'*16,candidate)
    server.ui.store.record('a'*16,'b'*16,datetime(2026,11,16,22,42,tzinfo=timezone.utc))
    status,body,_=request('/art')
    assert status==200 and 'A &lt;painting&gt;' in body and 'Times skipped' in body
    assert '16 Nov 17:42' in body
    assert request('/art/ignore',{'identity':'met:123'})[0]==403
    assert request('/art/ignore',{'csrf':token,'identity':'met:123'})[0]==303
    assert load_config(root/'settings.toml').ignored==('met:123',)
    assert server.ui.store.spares()[0][4]
    assert '# Quiet hours' in (root/'settings.toml').read_text()
    assert len(list((root/'backups').glob('*.toml')))==1
    assert 'A &lt;painting&gt;' in request('/art')[1]
    assert request('/art/unignore',{'csrf':token,'identity':'met:123'})[0]==303
    assert load_config(root/'settings.toml').ignored==()
    assert not server.ui.store.spares()[0][4]
    assert request('/art/ignore',{'csrf':token,'identity':'invalid'})[0]==400
    assert request('/art/ignore')[0]==405


def test_carryovers(web):
    request,server,root,token=ready(web,'/settings')
    body=request('/settings')[1]
    for name in ('The Met','Art Institute of Chicago','Cleveland Museum of Art','Rijksmuseum'): assert name in body
    status,body,_=request('/settings',{'csrf':token,'action':'rows','seasons.count':'bad'})
    assert status==400 and 'name="location.zip"' in body and 'expected 1–50' in body


def test_calibration_upload_preview_apply_cancel(web,monkeypatch):
    from ventanal import calibration_ui
    request,server,root,token=ready(web)
    image=simulate(chart(),[(26,26,37),(135,146,151),(150,146,59),(69,28,27),(39,66,122),(58,84,74)])
    data=BytesIO(); image.save(data,'PNG')
    (status,body),session,cookie=upload(server,None,data.getvalue())
    assert status==200 and 'TL, TR, BR, BL' in body and 'orientation' in body
    assert session_request(server,cookie,'/calibration/photo')[0]==200
    values={'csrf':session['csrf'],'corners':'0,0 1199,0 1199,899 0,899','preview_width':'1200','preview_height':'900'}
    monkeypatch.setattr(calibration_ui,'render_comparison',lambda cfg,colours,directory: None)
    status,body=session_request(server,cookie,'/calibration/measure',values)
    assert status==200 and b'Apply' in body and b'Raw' in body and b'Current palette' in body
    assert body.index("3 ·".encode()) < body.index("4 ·".encode())
    before=(root/'settings.toml').read_bytes()
    assert session_request(server,cookie,'/calibration/apply',{'csrf':'wrong'})[0]==403
    assert (root/'settings.toml').read_bytes()==before
    assert session_request(server,cookie,'/calibration/apply',{'csrf':session['csrf']})[0]==303
    assert load_config(root/'settings.toml').panel_rgb[0]==(26,26,37)
    assert '# Quiet hours' in (root/'settings.toml').read_text()
    assert session_request(server,cookie,'/calibration/photo')[0]==404
    (status,_),session,cookie=upload(server,None,data.getvalue())
    assert status==200
    assert session_request(server,cookie,'/calibration/cancel',{'csrf':session['csrf']})[0]==303
    assert session_request(server,cookie,'/calibration/photo')[0]==404


@pytest.mark.parametrize(('filename','kind','data'),[('photo.heic','image/heic',b'fake'),('photo.dng','image/tiff',b'fake'),('photo.png','image/png',b'bad'),('photo.gif','image/gif',b'GIF89a')])
def test_upload_validation(web,filename,kind,data):
    _,server,_,_=ready(web)
    (status,body),_,_=upload(server,None,data,filename,kind)
    assert status==400 and 'original RAW' in body


def test_upload_csrf_size_and_auth(web):
    _,server,_,_=ready(web)
    assert upload(server,'wrong',b'bad')[0][0]==403
    assert upload(server,None,b'',length=150*1024*1024+65537)[0][0]==413
    server.ui.auth.set_password('fixture password','fixture password')
    assert session_request(server,'invalid','/calibration/photo')[0]==303


def test_upload_exif_and_expiry(web):
    from ventanal.calibration_ui import CalibrationFiles
    _,server,root,_=ready(web)
    image=Image.new('RGB',(80,40),'white'); exif=Image.Exif(); exif[274]=6
    data=BytesIO(); image.save(data,'JPEG',exif=exif)
    (status,_),session,cookie=upload(server,None,data.getvalue(),'photo.jpg','image/jpeg')
    assert status==200
    files=CalibrationFiles(root/'cache',session['csrf'])
    with Image.open(files.directory/'photo.png') as normalized: assert normalized.size==(40,80) and not normalized.getexif()
    assert (files.directory/'photo.png').stat().st_mode & 0o777==0o600
    files.cleanup(now=time.time()+86401)
    assert not files.directory.exists()


def test_cli_paper(cli_config,tmp_path,capsys):
    from test_calibrate import KNOWN
    from ventanal.cli import main
    import tomllib
    data=np.asarray(simulate(chart(),KNOWN)).copy(); data[:30,50:100]=(171,181,206)
    path=tmp_path/'chart.png'; Image.fromarray(data).save(path)
    main(['calibrate',str(path),'--corners','0,0 1599,0 1599,1199 0,1199','--paper','50,0,50,30'])
    from ventanal.calibrate import white_balance
    assert tomllib.loads(capsys.readouterr().out)['panel']['black']=='#%02x%02x%02x'%white_balance(KNOWN,(171,181,206))[0]


def test_expiry_is_from_upload_and_inaccessible_across_sessions(web):
    from ventanal.calibration_ui import CalibrationFiles
    import os
    _,server,root,_=ready(web)
    data=BytesIO(); Image.new('RGB',(10,10),'white').save(data,'PNG')
    (_, _),session,cookie=upload(server,None,data.getvalue())
    _,other_cookie=server.ui.auth.issue(True)
    assert session_request(server,other_cookie,'/calibration/photo')[0]==404
    files=CalibrationFiles(root/'cache',session['csrf'])
    stamp=time.time()-86401
    os.utime(files.directory/'photo.png',(stamp,stamp))
    assert not files.valid()
    files.cleanup()
    assert not files.directory.exists()


def test_chart_restore_keeps_slot_identity(setup_job,monkeypatch):
    from ventanal import jobs
    from ventanal.calibration_ui import show_chart
    from ventanal.manifest import read_manifest
    from test_jobs import NOW
    from datetime import timedelta
    import httpx
    monkeypatch.setattr(jobs,'render_art',lambda image,c:(object(),{'title':c.title,'source':c.source,'object_id':c.object_id}))
    with httpx.Client() as client:
        jobs.tick(setup_job,NOW,client)
        state=(setup_job.cache_dir/'state.json').read_bytes()
        show_chart(setup_job); show_chart(setup_job)
        assert read_manifest(setup_job.output_dir)['screens']['art']['credit']['title']=='Calibration chart'
        monkeypatch.setattr(jobs.ArtPool,'pick_next',lambda *a,**k:pytest.fail('restore selected new art'))
        assert jobs.tick(setup_job,NOW+timedelta(minutes=1),client,force_art=True)['art']
    assert read_manifest(setup_job.output_dir)['screens']['art']['credit']['object_id']=='1'
    assert (setup_job.cache_dir/'state.json').read_bytes()==state


def test_comparison_rerenders_both_palettes(web,monkeypatch):
    from ventanal.calibration_ui import render_comparison
    from ventanal.art.sources import Candidate
    from ventanal.art.pool import ArtPool
    from ventanal.manifest import write_manifest
    from ventanal.art import sources,render
    from ventanal.quantize import Canvas,Method
    _,server,root,_=ready(web)
    cfg=load_config(root/'settings.toml')
    ArtPool(root/'cache/art.sqlite3').add([Candidate('met','1','Art','Artist','1900','Met','https://example.test',1600,1200)],'winter')
    write_manifest(root,{'version':1,'screens':{'art':{'id':'a'*16,'credit':{'source':'met','object_id':'1'}}}})
    monkeypatch.setattr(sources,'fetch_image',lambda *a:Image.new('RGB',(16,12)))
    monkeypatch.setattr(render,'render_art',lambda *a:(Canvas((16,12),fill=(240,240,240),method=Method.SOLID),{}))
    directory=root/'comparison'; directory.mkdir()
    colours=[(1,2,3),(255,255,255),(255,200,0),(200,0,0),(0,0,200),(0,100,0)]
    render_comparison(cfg,colours,directory)
    with Image.open(directory/'before.png') as before,Image.open(directory/'after.png') as after:
        assert before.getpixel((0,0))==cfg.panel_rgb[1]
        assert after.getpixel((0,0))==(255,255,255)


def test_background_requests_all_frames(web,monkeypatch):
    import subprocess
    _,server,_,_=ready(web)
    seen=[]
    def run(args,**kwargs):
        seen.extend(args)
        return subprocess.CompletedProcess(args,0,b'{"art":true,"weather":true,"spares":true}')
    monkeypatch.setattr('ventanal.ui.subprocess.run',run)
    server.ui._rerender()
    assert all(flag in seen for flag in ('--force-art','--force-weather','--force-spares'))



def test_background_reports_failed_spares(web,monkeypatch):
    import subprocess
    _,server,_,_=ready(web)
    monkeypatch.setattr('ventanal.ui.subprocess.run',lambda args,**kwargs:subprocess.CompletedProcess(args,0,b'{"art":true,"weather":true,"spares":false}'))
    with pytest.raises(RuntimeError,match='incomplete'): server.ui._rerender()


def test_upload_actual_file_size_and_supported_formats(web,monkeypatch):
    from ventanal import calibration_ui
    _,server,_,_=ready(web)
    for kind,suffix in [('JPEG','jpg'),('PNG','png'),('TIFF','tiff')]:
        data=BytesIO(); Image.new('RGB',(10,12),'white').save(data,kind)
        monkeypatch.setattr(calibration_ui,'MAX_UPLOAD',len(data.getvalue()))
        assert upload(server,None,data.getvalue(),f'chart.{suffix}')[0][0]==200
        assert upload(server,None,data.getvalue()+b'x',f'chart.{suffix}')[0][0]==413
    assert upload(server,None,b'GIF89a','chart.png')[0][0]==400


def test_chart_routes_require_post_and_csrf(web,monkeypatch):
    from ventanal import calibration_ui
    request,server,root,token=ready(web)
    seen=[]
    monkeypatch.setattr(calibration_ui,'show_chart',lambda cfg:seen.append('chart'))
    assert request('/calibration/chart')[0]==405
    assert request('/calibration/chart',{'csrf':'wrong'})[0]==403
    assert not seen
    assert request('/calibration/chart',{'csrf':token})[0]==303
    assert seen==['chart']
    assert request('/calibration/restore',{'csrf':token})[0]==303
    assert request('/calibration/apply',{'csrf':token})[0]==400
    assert request('/calibration/unknown')[0]==404
    assert request('/art/unknown')[0]==404


def test_upload_measure_paper_balances_and_rejects_bad_corners(web,monkeypatch):
    from ventanal import calibration_ui
    request,server,root,token=ready(web)
    image=simulate(chart(),[(26,26,37),(135,146,151),(150,146,59),(69,28,27),(39,66,122),(58,84,74)])
    image.paste((171,181,206),(50,0,100,30))
    data=BytesIO(); image.save(data,'PNG')
    (_, _),session,cookie=upload(server,None,data.getvalue())
    monkeypatch.setattr(calibration_ui,'render_comparison',lambda *a:None)
    values={'csrf':session['csrf'],'corners':'0,0 1199,0 1199,899 0,899','paper':'40,2,30,15'}
    status,body=session_request(server,cookie,'/calibration/measure',values)
    assert status==200 and b'Balanced' in body and b'#c1c5b3' in body
    files=calibration_ui.CalibrationFiles(root/'cache',session['csrf'])
    assert json.loads((files.directory/'measurement.json').read_text())['balanced'][0]==[42,39,46]
    values['corners']='0,0 0,0 0,0 0,0'
    assert session_request(server,cookie,'/calibration/measure',values)[0]==400


def test_nested_multipart_is_rejected_without_disconnect(web):
    _,server,_,_=ready(web)
    session,cookie=server.ui.auth.issue(True)
    body=('--outer\r\nContent-Disposition: form-data; name="photo"; filename="photo.png"\r\nContent-Type: multipart/mixed; boundary=inner\r\n\r\n'
          '--inner\r\nContent-Type: image/png\r\n\r\nbad\r\n--inner--\r\n--outer--\r\n').encode()
    c=HTTPConnection(*server.server_address)
    c.request('POST','/calibration/upload',body,{'Cookie':f'ventanal={cookie}','X-CSRF-Token':session['csrf'],'Content-Type':'multipart/form-data; boundary=outer'})
    response=c.getresponse(); response.read(); c.close()
    assert response.status==400


def test_calibration_failure_logged_and_activity(web, monkeypatch, caplog):
    from ventanal import calibration_ui
    request, server, _, token = ready(web)
    def fail(cfg): raise RuntimeError('fixture render failure')
    monkeypatch.setattr(calibration_ui, 'show_chart', fail)
    status, body, _ = request('/calibration/chart', {'csrf': token})
    assert status == 400 and 'Unable to complete calibration' in body
    assert 'fixture render failure' in caplog.text
    assert any(row['kind'] == 'calibration error' for row in server.ui.store.recent())


@pytest.mark.parametrize('content', ['{broken', '{}', 'null', '{"raw":null,"balanced":null}', '{"raw":[[999,0,0]],"balanced":null}'])
def test_corrupt_measurement_returns_logged_400(web, caplog, content):
    from ventanal.calibration_ui import CalibrationFiles
    request, server, root, token = ready(web)
    files = CalibrationFiles(root/'cache', token)
    data = BytesIO(); Image.new('RGB', (10, 10)).save(data, 'PNG')
    files.upload(data.getvalue(), 'photo.png')
    (files.directory/'measurement.json').write_text(content)
    assert request('/calibration')[0] == 400
    assert 'measurement' in caplog.text.lower()
    assert any(row['kind'] == 'calibration error' for row in server.ui.store.recent())
    assert request('/calibration/apply', {'csrf': token})[0] == 400


def test_measurement_does_not_hold_ui_lock(web, monkeypatch):
    from threading import Event, Thread
    from ventanal import calibration_ui
    request, server, _, token = ready(web)
    entered, finish = Event(), Event()
    def measure(*args):
        entered.set()
        assert finish.wait(5)
    monkeypatch.setattr(calibration_ui.CalibrationFiles, 'measure', measure)
    worker = Thread(target=lambda: request('/calibration/measure', {'csrf': token}))
    worker.start()
    try:
        assert entered.wait(2)
        acquired = server.ui.lock.acquire(timeout=.5)
        if acquired: server.ui.lock.release()
        assert acquired
        assert session_request(server, '', '/settings')[0] == 200
    finally:
        finish.set(); worker.join(3)


def test_unignore_preserves_independent_skip_and_rerender_reasons(web):
    from ventanal.art.sources import Candidate
    from ventanal.art_ui import update_ignored
    _, server, _, _ = ready(web, '/art')
    store = server.ui.store
    candidate = Candidate('met', '1', 'Art', 'Artist', '1900', 'Met', 'https://example.test', 1600, 1200)
    entry = {'id': 'a'*16, 'url': '/frames/a.vnf'}
    store.put_spare(0, entry, candidate)
    update_ignored(server.ui, 'met:1', True)
    store.put_spare(0, {**entry, 'id': 'b'*16}, candidate, replace_due=True)
    update_ignored(server.ui, 'met:1', False)
    assert not store.spares()[0][4]  # palette re-render retains ignore provenance
    store.put_spare(0, entry, candidate, replace_due=True)
    update_ignored(server.ui, 'met:1', True)
    update_ignored(server.ui, 'met:1', False)
    assert store.spares()[0][4]  # an already queued replacement survives
    store.put_spare(0, entry, candidate)
    update_ignored(server.ui, 'met:1', True)
    store.register('c'*16, candidate)
    assert store.record('c'*16, entry['id'], datetime.now(timezone.utc))
    update_ignored(server.ui, 'met:1', False)
    assert store.spares()[0][4]  # a later board skip is independent too


def test_published_previews_preserve_every_pixel_and_slot(web):
    from ventanal.art.sources import Candidate
    from ventanal.frame import encode_frame
    from ventanal.manifest import publish
    from ventanal.art.render import ART_READOUT
    from ventanal.status import preview
    _, server, root, _ = ready(web, '/art')
    cfg = load_config(root/'settings.toml')
    paths = []
    for slot in range(4):
        indices = np.full((1200, 1600), slot, dtype=np.uint8)
        indices[:, :30] = 4; indices[:, -30:] = 5
        indices[:20, :] = 0; indices[-20:, :] = 1
        entry = publish(cfg.output_dir, 'art' if slot == 0 else f'spare{slot-1}',
                        encode_frame(indices), ART_READOUT, {}, simulate(indices, cfg.panel_rgb), datetime.now(timezone.utc))
        path = preview(cfg, entry); paths.append(path)
        with Image.open(path) as picture:
            assert picture.size == (1600, 1200)
            np.testing.assert_array_equal(np.asarray(picture), np.asarray(simulate(indices, cfg.panel_rgb)))
    assert len(set(paths)) == 4


@pytest.mark.parametrize(('mode', 'proof', 'status'), [
    ('unset', 'valid', 403), ('password', 'anonymous', 303), ('none', 'missing', 403),
    ('none', 'wrong', 403), ('none', 'duplicate', 403), ('none', 'no-cookie', 403),
])
def test_large_upload_checks_access_before_reading_body(web, mode, proof, status):
    _,server,_=web
    if mode == 'none':
        server.ui.auth.none()
    elif mode == 'password':
        server.ui.auth.set_password('fixture password','fixture password')
    session, cookie = server.ui.auth.issue(False)
    connection=HTTPConnection(*server.server_address,timeout=2)
    try:
        connection.putrequest('POST','/calibration/upload')
        if proof != 'no-cookie':
            connection.putheader('Cookie', f'ventanal={cookie}')
        if proof != 'missing':
            connection.putheader('X-CSRF-Token', 'wrong' if proof == 'wrong' else session['csrf'])
        if proof == 'duplicate':
            connection.putheader('X-CSRF-Token', session['csrf'])
        connection.putheader('Content-Type','multipart/form-data; boundary=fixture')
        connection.putheader('Content-Length',str(150*1024*1024))
        # No body is sent: attempting to consume it would hit the socket timeout.
        connection.endheaders()
        response=connection.getresponse()
        assert response.status == status
        if status == 303:
            assert response.getheader('Location') == '/login'
        response.read()
    finally:
        connection.close()


def test_pre_upgrade_photo_measurement_requests_reupload(web):
    from ventanal.calibration_ui import CalibrationFiles
    _, server, root, _ = ready(web)
    data = BytesIO()
    Image.new('RGB', (10, 10)).save(data, 'PNG')
    (_, _), session, cookie = upload(server, None, data.getvalue())
    files = CalibrationFiles(root / 'cache', session['csrf'])
    (files.directory / 'linear.npy').unlink()
    (files.directory / 'source.json').unlink()
    status, body = session_request(server, cookie, '/calibration/measure',
                                   {'csrf': session['csrf'], 'corners': '0,0 9,0 9,9 0,9'})
    assert status == 400
    assert b'Please upload the photo again' in body
