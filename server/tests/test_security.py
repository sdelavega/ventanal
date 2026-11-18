# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Stephen de la Vega
import hashlib
import hmac
from contextlib import nullcontext
from ipaddress import ip_address
import json
import ssl
import subprocess
import sys
from http.client import HTTPConnection, HTTPSConnection
from threading import Thread
from pathlib import Path
from types import SimpleNamespace
from cryptography import x509
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.x509.oid import ExtendedKeyUsageOID
import pytest
from ventanal.security import Security, canonical
from ventanal.serve import make_server


def proof(key, path, counter, method='GET'):
    return {'Authorization': 'Ventanal '+hmac.new(key, f'{method}\n{path}\n{counter}'.encode(), hashlib.sha256).hexdigest(), 'X-Ventanal-Counter':str(counter)}


def test_cross_language_request_proof(tmp_path):
    fixture = Path(__file__).parents[2] / 'firmware/test/fixtures/request-proof.json'
    vector = json.loads(fixture.read_text(encoding='utf-8'))
    method, path, counter = vector['method'], vector['path'], vector['counter']
    key = bytes(32)
    headers = {'Authorization': 'Ventanal ' + vector['mac'], 'X-Ventanal-Counter': str(counter)}
    assert proof(key, path, counter, method) == headers

    security = Security(tmp_path)
    security.enable(['127.0.0.1'])
    security.state['device_key'] = key.hex()
    legacy_mac = hmac.new(key, f'{method}{path}{counter}'.encode(), hashlib.sha256).hexdigest()
    assert not security.accept(method, path, {**headers, 'Authorization': 'Ventanal ' + legacy_mac})
    assert security.state['counter'] == 0
    assert security.accept(method, path, headers)


def test_certificate_proofs_rotation_and_persistence(tmp_path):
    s=Security(tmp_path)
    assert not s.ready
    s.enable(['127.0.0.1','test.local'])
    cert=x509.load_pem_x509_certificate(s.certificate.encode())
    assert isinstance(cert.public_key(),ec.EllipticCurvePublicKey)
    assert cert.public_key().curve.name=='secp256r1'
    assert (cert.not_valid_after_utc-cert.not_valid_before_utc).days >= 3650
    san = cert.extensions.get_extension_for_class(x509.SubjectAlternativeName).value
    assert san.get_values_for_type(x509.DNSName) == ['test.local']
    assert san.get_values_for_type(x509.IPAddress) == [ip_address('127.0.0.1')]
    basic = cert.extensions.get_extension_for_class(x509.BasicConstraints).value
    assert not basic.ca and basic.path_length is None
    usage = cert.extensions.get_extension_for_class(x509.KeyUsage).value
    assert usage.digital_signature and usage.key_encipherment and not usage.key_cert_sign
    eku = cert.extensions.get_extension_for_class(x509.ExtendedKeyUsage).value
    assert list(eku) == [ExtendedKeyUsageOID.SERVER_AUTH]
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


def test_legacy_certificate_preserved_until_cli_regeneration(tmp_path, capsys, monkeypatch):
    from ventanal.cli import main

    state = tmp_path / 'state'
    config = tmp_path / 'settings.toml'
    example = Path(__file__).parents[1] / 'ventanal.example.toml'
    config.write_text(example.read_text().replace('/var/cache/ventanal', str(state)))
    names = ['127.0.0.1', 'test.local']
    monkeypatch.setattr('ventanal.security.host_names', lambda: names)
    security = Security(state)
    security.enable(names)
    cert = x509.load_pem_x509_certificate(security.certificate.encode())
    signer = serialization.load_pem_private_key(security.state['private_key'].encode(), None)
    legacy = (x509.CertificateBuilder().subject_name(cert.subject).issuer_name(cert.issuer)
              .public_key(signer.public_key()).serial_number(cert.serial_number)
              .not_valid_before(cert.not_valid_before_utc).not_valid_after(cert.not_valid_after_utc)
              .add_extension(cert.extensions.get_extension_for_class(x509.SubjectAlternativeName).value, False)
              .add_extension(x509.BasicConstraints(ca=True, path_length=0), True)
              .sign(signer, hashes.SHA256()))
    security.state['certificate'] = legacy.public_bytes(serialization.Encoding.PEM).decode()
    assert security.accept('GET', '/manifest.json', proof(security.key, '/manifest.json', 7))
    original = security.path.read_bytes()
    assert Security(state).certificate == security.certificate
    command = ['regenerate-certificate', '--config', str(config)]
    with pytest.raises(SystemExit):
        main(command)
    assert security.path.read_bytes() == original

    main([*command, '--confirm'])
    rotated = Security(state)
    assert rotated.certificate != security.certificate
    assert rotated.key == security.key and rotated.state['counter'] == 7
    assert rotated.fingerprint in capsys.readouterr().out
    new_cert = x509.load_pem_x509_certificate(rotated.certificate.encode())
    assert not new_cert.extensions.get_extension_for_class(x509.BasicConstraints).value.ca

    assert rotated.state['previous'] == {
        'certificate': security.certificate, 'private_key': security.state['private_key'],
    }
    assert rotated.path.stat().st_mode & 0o777 == 0o600
    assert rotated.accept('GET', '/manifest.json', proof(security.key, '/manifest.json', 9))
    restore = ['restore-certificate', '--config', str(config)]
    before_restore = rotated.path.read_bytes()
    with pytest.raises(SystemExit):
        main(restore)
    assert rotated.path.read_bytes() == before_restore

    main([*restore, '--confirm'])
    restored = Security(state)
    assert restored.fingerprint == security.fingerprint
    assert restored.state['private_key'] == security.state['private_key']
    assert restored.fingerprint in capsys.readouterr().out
    assert restored.key == security.key and restored.state['counter'] == 9
    assert not restored.accept('GET', '/manifest.json', proof(security.key, '/manifest.json', 9))
    assert restored.accept('GET', '/manifest.json', proof(security.key, '/manifest.json', 10))
    assert not restored.can_restore and restored.path.stat().st_mode & 0o777 == 0o600
    with pytest.raises(SystemExit):
        main([*restore, '--confirm'])
    assert 'No previous certificate is available' in capsys.readouterr().err


def test_certificate_commands_refuse_while_serve_holds_state_lock(tmp_path, monkeypatch):
    from ventanal import cli

    state = tmp_path / 'state'
    config = tmp_path / 'settings.toml'
    example = Path(__file__).parents[1] / 'ventanal.example.toml'
    config.write_text(example.read_text().replace('/var/cache/ventanal', str(state)))
    security = Security(state)
    security.enable(['127.0.0.1'])
    security.regenerate(['127.0.0.1'])
    path = '/manifest.json'

    def serve_forever():
        assert security.accept('GET', path, proof(security.key, path, 12))
        original = security.path.read_bytes()
        for command in ('regenerate-certificate', 'restore-certificate'):
            result = subprocess.run(
                [sys.executable, '-m', 'ventanal.cli', command, '--config', str(config), '--confirm'],
                capture_output=True, text=True, timeout=10,
            )
            assert result.returncode == 2
            assert 'Stop ventanal-serve.service' in result.stderr
            assert security.path.read_bytes() == original

    monkeypatch.setattr(cli, 'make_server', lambda *args, **kwargs: nullcontext(
        SimpleNamespace(serve_forever=serve_forever)))
    cli.main(['serve', '--config', str(config)])
    cli.main(['restore-certificate', '--config', str(config), '--confirm'])
    assert Security(state).state['counter'] == 12


def test_failed_certificate_restore_keeps_active_identity_and_backup(tmp_path, monkeypatch):
    security = Security(tmp_path)
    security.enable(['127.0.0.1'])
    security.regenerate(['127.0.0.1'])
    before = security.path.read_bytes()
    context = security.context
    state = security.state
    from ventanal import auth
    replace = auth.os.replace

    def fail_state_write(source, destination):
        if destination == security.path:
            raise OSError('injected replace failure')
        replace(source, destination)

    monkeypatch.setattr(auth.os, 'replace', fail_state_write)
    with pytest.raises(OSError, match='injected replace failure'):
        security.restore()
    assert security.path.read_bytes() == before
    assert security.context is context and security.state is state
    assert security.can_restore


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


def test_cross_language_manifest_controls():
    fixture = Path(__file__).parents[2] / 'firmware/test/fixtures/manifest-controls.json'
    controls = ''.join(map(chr, range(32))) + '\x7f'
    manifest = {
        'version': 1,
        'screens': {'art': {
            'url': '/frames/art.vnf',
            'sha256': '0' * 64,
            'credit': {'title': 'Árbol / "x" \\' + controls, 'key' + controls: 'value'},
        }},
    }
    manifest['sig'] = hmac.new(bytes(32), canonical(manifest), hashlib.sha256).hexdigest()
    assert json.loads(fixture.read_text(encoding='utf-8')) == manifest


@pytest.mark.parametrize('document', [
    {'version': 1.0},
    {'screens': {'art': {'readout': {'x': 1.0}}}},
    {'spares': [{'size': 1.0}]},
])
def test_signed_manifest_rejects_floats(tmp_path, document):
    with pytest.raises(ValueError, match='Floats are unsupported'):
        Security(tmp_path).manifest(document, tmp_path)


@pytest.mark.parametrize('mode', ['static', 'plain', 'signed'])
def test_served_manifest_budget_with_four_long_credits(tmp_path, monkeypatch, mode):
    from ventanal.extras import registry

    config_path = None
    cache_dir = None
    if mode != 'static':
        config_path = tmp_path / 'settings.toml'
        cache_dir = tmp_path / 'cache'
        example = Path(__file__).parents[1] / 'ventanal.example.toml'
        config_path.write_text(example.read_text().replace('/var/lib/ventanal', str(tmp_path))
                               .replace('/var/cache/ventanal', str(cache_dir)))
    extras = ('extra-' + 'x' * 42, 'extra-' + 'y' * 42)
    monkeypatch.setattr(registry, 'active', lambda *a, **kw: [SimpleNamespace(screens=extras)])
    monkeypatch.setattr('ventanal.solar.licence.verify', lambda *a: SimpleNamespace(ok=True))
    (tmp_path / 'frames').mkdir()

    long_credit = dict.fromkeys(('title', 'secondary', 'artist', 'date', 'museum'),
                               '𐀀' * 200 + '\x00\x1f\x7f\x9f')

    def entry(name, credit=None):
        ident = 'a' * 16
        url = f'/frames/{name}-{ident}.vnf'
        (tmp_path / url.lstrip('/')).write_bytes(b'frame')
        return {'id': ident, 'url': url, 'size': 960016, 'rendered': '2026-11-15T14:30:00-05:00',
                'readout': {'x': 1020, 'y': 0, 'w': 580, 'h': 104, 'style': 'shade'}, 'credit': credit}

    order = ('art', 'weather', 'solar', *extras)
    manifest = {'version': 1, 'screens': {name: entry(name, long_credit if name == 'art' else None)
                                         for name in order},
                'screen_order': list(order), 'quiet': {'start': '22:00', 'end': '07:00'},
                'spares': [entry(f'spare{i}', long_credit) for i in range(3)]}
    original = json.dumps(manifest)
    assert len(json.dumps(manifest, ensure_ascii=False, separators=(',', ':')).encode()) > 8192
    path = tmp_path / 'manifest.json'
    path.write_text(original)
    srv = make_server(tmp_path, '127.0.0.1', 0, cache_dir=cache_dir, config_path=config_path,
                      https_host='127.0.0.1', https_port=0)
    thread = Thread(target=srv.serve_forever, daemon=True)
    thread.start()
    try:
        headers = {}
        if mode == 'signed':
            srv.ui.enable_security()
            srv.ui.auth.set_password('test password', 'test password')
            ctx = ssl.create_default_context(cadata=srv.ui.security.certificate)
            conn = HTTPSConnection(*srv.tls_server.server_address, context=ctx)
            headers = proof(srv.ui.security.key, '/manifest.json', 1)
        else:
            conn = HTTPConnection(*srv.server_address)
        try:
            conn.request('GET', '/manifest.json', headers=headers)
            response = conn.getresponse()
            body = response.read()
            assert response.status == 200
            assert int(response.getheader('Content-Length')) == len(body) <= 6144
        finally:
            conn.close()
        served = json.loads(body)
        assert served['screen_order'] == list(order)
        assert served['quiet'] == manifest['quiet']
        assert len(served['spares']) == 3
        entries = [*served['screens'].values(), *served['spares']]
        assert all('credit' not in entry for entry in entries)
        if mode == 'signed':
            assert all(entry['sha256'] == hashlib.sha256(b'frame').hexdigest() for entry in entries)
            sig = served.pop('sig')
            assert sig == hmac.new(srv.ui.security.key, canonical(served), hashlib.sha256).hexdigest()
        assert path.read_text() == original
    finally:
        srv.shutdown()
        srv.server_close()
        thread.join()


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
        srv.ui.auth.set_password('test password','test password')
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
