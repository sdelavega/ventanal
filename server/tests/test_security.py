# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Stephen de la Vega
import hashlib
import hmac
import json
import ssl
from http.client import HTTPConnection, HTTPSConnection
from threading import Thread
from pathlib import Path
from cryptography import x509
from cryptography.hazmat.primitives.asymmetric import ec
from ventanal.security import Security, canonical
from ventanal.serve import make_server


def proof(key, path, counter, method='GET'):
    return {'Authorization': 'Ventanal '+hmac.new(key, (method+path+str(counter)).encode(), hashlib.sha256).hexdigest(), 'X-Ventanal-Counter':str(counter)}


def test_certificate_proofs_rotation_and_persistence(tmp_path):
    s=Security(tmp_path)
    assert not s.ready
    s.enable(['127.0.0.1','test.local'])
    cert=x509.load_pem_x509_certificate(s.certificate.encode())
    assert isinstance(cert.public_key(),ec.EllipticCurvePublicKey)
    assert cert.public_key().curve.name=='secp256r1'
    assert (cert.not_valid_after_utc-cert.not_valid_before_utc).days >= 3650
    assert 'test.local' in cert.extensions.get_extension_for_class(x509.SubjectAlternativeName).value.get_values_for_type(x509.DNSName)
    assert (tmp_path/'security.json').stat().st_mode & 0o777 == 0o600
    key=s.key
    path='/manifest.json?bat=95&skips=a~b'
    assert not s.accept('GET',path,{})
    assert not s.accept('GET',path,proof(b'x'*32,path,1))
    assert s.accept('GET',path,proof(key,path,1))
    assert not Security(tmp_path).accept('GET',path,proof(key,path,1))
    assert not s.accept('GET',path+'&fw=changed',proof(key,path,2))
    assert not s.accept('GET',path,proof(key,path,0))
    assert not s.accept('GET',path,proof(key,path,2**64))
    assert s.accept('GET',path,proof(key,path,3))
    old=s.certificate
    s.regenerate(['127.0.0.1'])
    assert s.certificate != old and s.key == key
    s.repair()
    assert s.key != key and not s.accept('GET',path,proof(key,path,4))
    assert s.accept('GET',path,proof(s.key,path,1))


def test_canonical_signed_frames(tmp_path):
    s=Security(tmp_path/'state'); s.enable(['127.0.0.1'])
    (tmp_path/'frames').mkdir(); (tmp_path/'frames/a.vnf').write_bytes(b'frame')
    m={'version':1,'screens':{'art':{'url':'/frames/a.vnf','credit':{'title':'Árbol'}}}}
    signed=s.manifest(m,tmp_path)
    assert signed['screens']['art']['sha256']==hashlib.sha256(b'frame').hexdigest()
    sig=signed.pop('sig')
    assert sig==hmac.new(s.key,canonical(signed),hashlib.sha256).hexdigest()
    assert canonical({'b':1,'a':'Á'}) == b'{"a":"\xc3\x81","b":1}'
    assert 'sha256' not in m['screens']['art']


def test_https_and_plain_protection(tmp_path):
    cfg=tmp_path/'settings.toml'
    example=Path(__file__).parents[1]/'ventanal.example.toml'
    cfg.write_text(example.read_text().replace('/var/lib/ventanal',str(tmp_path)).replace('/var/cache/ventanal',str(tmp_path/'cache')))
    (tmp_path/'manifest.json').write_text('{"version":1,"screens":{}}')
    srv=make_server(tmp_path,'127.0.0.1',0,cache_dir=tmp_path/'cache',config_path=cfg,https_port=0)
    thread=Thread(target=srv.serve_forever,daemon=True);thread.start()
    def request(conn,path='/',headers=None):
        conn.request('GET',path,headers=headers or {}); r=conn.getresponse(); body=r.read(); return r,body
    try:
        r,_=request(HTTPConnection(*srv.server_address),'/manifest.json'); assert r.status==200
        srv.ui.enable_security()
        srv.ui.auth.set_password('test','test')
        for path in ['/manifest.json','/frames/a.vnf','/previews/a.png','/%6danifest.json','/fr%61mes/a.vnf']:
            r,_=request(HTTPConnection(*srv.server_address),path); assert r.status==403
        r,_=request(HTTPConnection(*srv.server_address),'/login'); assert r.status==308 and r.getheader('Location').startswith('https://') and not r.getheader('Set-Cookie')
        ctx=ssl.create_default_context(cadata=srv.ui.security.certificate)
        def tls(): return HTTPSConnection("127.0.0.1",srv.tls_server.server_address[1],context=ctx)
        r,_=request(tls(),'/manifest.json'); assert r.status==403
        r,_=request(tls(),'/%6danifest.json'); assert r.status==403
        r,_=request(tls(),'/fr%61mes/a.vnf'); assert r.status==403
        headers=proof(srv.ui.security.key,'/manifest.json',1)
        r,body=request(tls(),'/manifest.json',headers); assert r.status==200 and 'sig' in json.loads(body)
        assert not r.will_close  # One pinned TLS connection can serve the whole wake.
        r,_=request(tls(),'/manifest.json',headers); assert r.status==403
        r,_=request(tls(),'/login'); assert r.status==200 and 'Secure' in r.getheader('Set-Cookie')
        r,_=request(tls(),'/security'); assert r.status==303
        conn=tls()
        conn.request('POST','/security',body='unread=body',headers={'Content-Type':'application/x-www-form-urlencoded'})
        r=conn.getresponse(); r.read()
        assert r.status==303 and r.getheader('Connection')=='close' and r.will_close
        conn=tls()
        conn.request('POST','/manifest.json',body='unread=body')
        r=conn.getresponse(); r.read()
        assert r.status==403 and r.getheader('Connection')=='close' and r.will_close
    finally:
        srv.shutdown();srv.server_close();thread.join()


def test_counter_storage_failure_is_rejected(tmp_path,monkeypatch):
    s=Security(tmp_path);s.enable(['127.0.0.1'])
    original=s._save
    def fail(): raise OSError('injected write failure')
    monkeypatch.setattr(s,'_save',fail)
    headers=proof(s.key,'/manifest.json',1)
    assert not s.accept('GET','/manifest.json',headers)
    assert s.state['counter']==0
    monkeypatch.setattr(s,'_save',original)
    assert s.accept('GET','/manifest.json',headers)


def test_lost_active_identity_never_reopens_plain_transport(tmp_path):
    import pytest
    s=Security(tmp_path);s.enable(['127.0.0.1'])
    (tmp_path/'security.json').unlink()
    with pytest.raises(RuntimeError,match='Transport identity missing'):
        Security(tmp_path)
