# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Stephen de la Vega
"""TLS identity, single-board pairing and durable replay protection.

Canonical JSON: UTF-8, sorted object keys, no insignificant whitespace, integer
numbers (the manifest schema has no floating point fields), no ASCII escaping.
"""
import copy
from datetime import datetime, timedelta, timezone
import hashlib
import hmac
import ipaddress
import json
from pathlib import Path
import re
import secrets
import socket
import ssl
from threading import RLock
from cryptography import x509
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.x509.oid import ExtendedKeyUsageOID, NameOID
from ventanal.auth import private_write
from ventanal.manifest import reject_manifest_floats


def canonical(value):
    return json.dumps(value, sort_keys=True, separators=(',', ':'), ensure_ascii=False, allow_nan=False).encode()


def host_names():
    names={socket.gethostname(),socket.getfqdn(),'localhost','127.0.0.1'}
    for name in tuple(names):
        try:
            names.update(row[4][0] for row in socket.getaddrinfo(name,None))
        except OSError:
            pass
    # Discover the selected LAN interface without sending any traffic.
    try:
        with socket.socket(socket.AF_INET,socket.SOCK_DGRAM) as sock:
            sock.connect(('192.0.2.1',9)); names.add(sock.getsockname()[0])
    except OSError:
        pass
    return sorted(names)


class Security:
    def __init__(self,directory):
        self.directory=Path(directory); self.directory.mkdir(parents=True,exist_ok=True)
        self.path=self.directory/'security.json'
        self.required=self.directory/'transport.required'
        if self.required.exists() and not self.path.exists():
            raise RuntimeError('Transport identity missing; restore the private state backup before serving.')
        self.lock=RLock()
        self.state=json.loads(self.path.read_text()) if self.path.exists() else None
        self.context=None
        if self.ready:
            self._context()
            if not self.required.exists(): private_write(self.required,b'1')

    @property
    def ready(self): return self.state is not None
    @property
    def key(self): return bytes.fromhex(self.state['device_key'])
    @property
    def certificate(self): return self.state['certificate']
    @property
    def fingerprint(self):
        return x509.load_pem_x509_certificate(self.certificate.encode()).fingerprint(hashes.SHA256()).hex()

    def _save(self): private_write(self.path,canonical(self.state))

    def _context(self):
        # ssl needs filenames; private temporary material is removed immediately.
        cert=self.directory/('tls-'+secrets.token_hex(8)+'.pem')
        key=cert.with_suffix('.key')
        try:
            private_write(cert,self.certificate.encode()); private_write(key,self.state['private_key'].encode())
            context=ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
            context.minimum_version=ssl.TLSVersion.TLSv1_2
            context.load_cert_chain(cert,key)
            self.context=context
        finally:
            cert.unlink(missing_ok=True); key.unlink(missing_ok=True)

    def enable(self,names=None):
        with self.lock:
            if self.ready: return
            self.state={'device_key':secrets.token_hex(32),'counter':0}
            try:
                self.regenerate(names)
                private_write(self.required,b'1')
            except Exception:
                self.state=None
                raise

    def regenerate(self,names=None):
        with self.lock:
            key=ec.generate_private_key(ec.SECP256R1())
            names=names or host_names()
            sans=[]
            for name in names:
                try: sans.append(x509.IPAddress(ipaddress.ip_address(name)))
                except ValueError: sans.append(x509.DNSName(name))
            subject=x509.Name([x509.NameAttribute(NameOID.COMMON_NAME,socket.gethostname())])
            now=datetime.now(timezone.utc)
            cert=(x509.CertificateBuilder().subject_name(subject).issuer_name(subject)
                  .public_key(key.public_key()).serial_number(x509.random_serial_number())
                  .not_valid_before(now-timedelta(minutes=5)).not_valid_after(now+timedelta(days=3653))
                  .add_extension(x509.SubjectAlternativeName(sans),critical=False)
                  .add_extension(x509.BasicConstraints(ca=False, path_length=None), critical=True)
                  .add_extension(x509.KeyUsage(
                      digital_signature=True, content_commitment=False, key_encipherment=True,
                      data_encipherment=False, key_agreement=False, key_cert_sign=False,
                      crl_sign=False, encipher_only=False, decipher_only=False), critical=True)
                  .add_extension(x509.ExtendedKeyUsage([ExtendedKeyUsageOID.SERVER_AUTH]), critical=False)
                  .sign(key,hashes.SHA256()))
            previous, context = self.state, self.context
            self.state={**self.state,'certificate':cert.public_bytes(serialization.Encoding.PEM).decode(),
                        'private_key':key.private_bytes(serialization.Encoding.PEM,serialization.PrivateFormat.PKCS8,serialization.NoEncryption()).decode()}
            try:
                self._context(); self._save()
            except Exception:
                self.state, self.context = previous, context
                raise

    def repair(self):
        with self.lock:
            previous=self.state
            self.state={**self.state,'device_key':secrets.token_hex(32),'counter':0}
            try: self._save()
            except OSError:
                self.state=previous; raise

    def accept(self,method,path,headers):
        counter=headers.get('X-Ventanal-Counter','')
        auth=headers.get('Authorization','')
        if not re.fullmatch(r'[1-9][0-9]{0,19}',counter) or int(counter)>=2**64: return False
        if not re.fullmatch(r'Ventanal [0-9a-f]{64}',auth): return False
        with self.lock:
            expected=hmac.new(self.key,f'{method}\n{path}\n{counter}'.encode(),hashlib.sha256).hexdigest()
            if int(counter)<=self.state['counter'] or not hmac.compare_digest(auth[9:],expected): return False
            previous=self.state['counter']; self.state['counter']=int(counter)
            try: self._save()
            except OSError:
                self.state['counter']=previous
                return False
            return True

    def manifest(self,manifest,root):
        result=copy.deepcopy(manifest); result.pop('sig',None)
        reject_manifest_floats(result)
        for entry in [*result.get('screens',{}).values(),*result.get('spares',[])]:
            url=entry['url']
            if not re.fullmatch(r'/frames/[A-Za-z0-9_-]+\.vnf',url): raise ValueError('Invalid frame path')
            path=root/url.lstrip('/')
            if path.resolve()!=path: raise ValueError('Invalid frame path')
            with path.open('rb') as f: entry['sha256']=hashlib.file_digest(f,'sha256').hexdigest()
        with self.lock:
            result['sig']=hmac.new(self.key,canonical(result),hashlib.sha256).hexdigest()
        return result
