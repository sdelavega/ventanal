# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Stephen de la Vega
"""TLS identity, device pairing and durable replay protection.

Canonical JSON: UTF-8, sorted object keys, no insignificant whitespace, integer
numbers (the manifest schema has no floating point fields), no ASCII escaping.
"""
import copy
from contextlib import contextmanager
from datetime import datetime, timedelta, timezone
import fcntl
import hashlib
import hmac
import ipaddress
import json
import os
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
from ventanal.config import normalize_hostname
from ventanal.manifest import reject_manifest_floats


def canonical(value):
    return json.dumps(value, sort_keys=True, separators=(',', ':'), ensure_ascii=False, allow_nan=False).encode()


class SecurityStateBusy(RuntimeError):
    pass


class PreviousCertificateHeld(ValueError):
    pass


@contextmanager
def security_state_lock(directory):
    directory = Path(directory)
    directory.mkdir(parents=True, exist_ok=True)
    fd = os.open(directory / '.security.lock', os.O_RDWR | os.O_CREAT, 0o600)
    try:
        try:
            fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError as exc:
            raise SecurityStateBusy(
                'Ventanal security state is in use. Stop ventanal-serve.service '
                '(or any manually running ventanal serve) and wait for any certificate command to finish.'
            ) from exc
        yield
    finally:
        os.close(fd)


def host_names():
    names={socket.gethostname(),socket.getfqdn(),'localhost','127.0.0.1'}
    for name in tuple(names):
        try:
            for _, _, _, canonical_name, address in socket.getaddrinfo(name, None, flags=socket.AI_CANONNAME):
                names.add(address[0])
                if canonical_name:
                    names.add(canonical_name)
        except OSError:
            pass
    # Discover the selected LAN interface without sending any traffic.
    try:
        with socket.socket(socket.AF_INET,socket.SOCK_DGRAM) as sock:
            sock.connect(('192.0.2.1',9)); names.add(sock.getsockname()[0])
    except OSError:
        pass
    return sorted(names)


def certificate_dns_names(certificate):
    cert = x509.load_pem_x509_certificate(certificate.encode())
    try:
        san = cert.extensions.get_extension_for_class(x509.SubjectAlternativeName).value
    except x509.ExtensionNotFound:
        return frozenset()
    return frozenset(filter(None, map(normalize_hostname, san.get_values_for_type(x509.DNSName))))


class Security:
    NO_PREVIOUS_CERTIFICATE = 'No previous certificate is available to restore.'

    def __init__(self,directory):
        self.directory=Path(directory); self.directory.mkdir(parents=True,exist_ok=True)
        self.path=self.directory/'security.json'
        self.required=self.directory/'transport.required'
        if self.required.exists() and not self.path.exists():
            raise RuntimeError('Transport identity missing; restore the private state backup before serving.')
        self.lock=RLock()
        self.state=json.loads(self.path.read_text()) if self.path.exists() else None
        self.context=None
        self.dns_names = frozenset()
        if self.ready:
            self.context = self._make_context(self.state)
            self.dns_names = certificate_dns_names(self.certificate)
            if not self.required.exists(): private_write(self.required,b'1')

    @property
    def ready(self): return self.state is not None
    @property
    def can_restore(self): return self.ready and 'previous' in self.state
    @property
    def key(self): return bytes.fromhex(self.state['device_key'])
    @property
    def certificate(self): return self.state['certificate']
    @property
    def fingerprint(self):
        return x509.load_pem_x509_certificate(self.certificate.encode()).fingerprint(hashes.SHA256()).hex()

    def _save(self): private_write(self.path,canonical(self.state))

    def _make_context(self, state):
        # ssl needs filenames; private temporary material is removed immediately.
        cert=self.directory/('tls-'+secrets.token_hex(8)+'.pem')
        key=cert.with_suffix('.key')
        try:
            private_write(cert,state['certificate'].encode()); private_write(key,state['private_key'].encode())
            context=ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
            context.minimum_version=ssl.TLSVersion.TLSv1_2
            context.load_cert_chain(cert,key)
            return context
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

    def regenerate(self,names=None, *, discard_previous=False):
        with self.lock:
            if self.can_restore and not discard_previous:
                raise PreviousCertificateHeld('A previous certificate is still saved for rollback.')
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
            state = self.state.copy()
            if 'certificate' in state:
                state['previous'] = {field: state[field] for field in ('certificate', 'private_key')}
            state['certificate'] = cert.public_bytes(serialization.Encoding.PEM).decode()
            state['private_key'] = key.private_bytes(
                serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8,
                serialization.NoEncryption()).decode()
            self._replace_identity(state)

    def restore(self):
        with self.lock:
            if not self.can_restore:
                return False
            state = self.state.copy()
            previous = state.pop('previous')
            for field in ('certificate', 'private_key'):
                state[field] = previous[field]
            self._replace_identity(state)
            return True

    def _replace_identity(self, state):
        context = self._make_context(state)
        dns_names = certificate_dns_names(state['certificate'])
        private_write(self.path, canonical(state))
        self.state, self.context, self.dns_names = state, context, dns_names

    def repair(self):
        with self.lock:
            previous=self.state
            self.state={**self.state,'device_key':secrets.token_hex(32),'counter':0}
            try: self._save()
            except OSError:
                self.state=previous; raise

    def pair_lcd(self, device, profile, *, rotate=False):
        if not re.fullmatch(r'[a-z0-9-]{1,32}', device):
            raise ValueError('Enter a pairing ID of 1–32 lowercase letters, numbers or hyphens.')
        with self.lock:
            devices = self.state.get('lcd_devices', {})
            if device in devices and not rotate:
                raise ValueError('That pairing ID already exists. Reveal or re-pair it below.')
            if rotate and device not in devices:
                raise ValueError('Unknown LCD pairing ID.')
            previous = self.state
            self.state = {**previous, 'lcd_devices': {**devices, device: {
                'profile': profile, 'device_key': secrets.token_hex(32), 'counter': 0,
            }}}
            try:
                self._save()
            except OSError:
                self.state = previous
                raise

    def lcd_pairings(self):
        with self.lock:
            return [(device, entry['profile']) for device, entry in
                    sorted(self.state.get('lcd_devices', {}).items())]

    def lcd_values(self, device):
        with self.lock:
            entry = self.state.get('lcd_devices', {}).get(device)
            if entry is None:
                raise ValueError('Unknown LCD pairing ID.')
            return entry['profile'], entry['device_key']

    def accept(self,method,path,headers):
        return self.authenticate(method, path, headers) is not None

    def authenticate(self, method, path, headers, profile=None):
        """Return the key that authenticated this request, for manifest signing."""
        counter=headers.get('X-Ventanal-Counter','')
        auth=headers.get('Authorization','')
        device = headers.get('X-Ventanal-Device')
        if not re.fullmatch(r'[1-9][0-9]{0,19}',counter) or int(counter)>=2**64: return None
        if not re.fullmatch(r'Ventanal [0-9a-f]{64}',auth): return None
        with self.lock:
            if profile is None:
                if device is not None:
                    return None
                entry = self.state
            else:
                entry = self.state.get('lcd_devices', {}).get(device)
                if entry is None or entry['profile'] != profile:
                    return None
            key = bytes.fromhex(entry['device_key'])
            expected = hmac.new(key, f'{method}\n{path}\n{counter}'.encode(), hashlib.sha256).hexdigest()
            if int(counter) <= entry['counter'] or not hmac.compare_digest(auth[9:], expected):
                return None
            previous = entry['counter']
            entry['counter'] = int(counter)
            try: self._save()
            except OSError:
                entry['counter'] = previous
                return None
            return key

    def manifest(self, manifest, root, *, key=None, profile=None):
        result=copy.deepcopy(manifest); result.pop('sig',None)
        reject_manifest_floats(result)
        frame_route = (r'/frames/[A-Za-z0-9_-]+\.vnf' if profile is None else
                       rf'/devices/{re.escape(profile)}/frames/[A-Za-z0-9_-]+\.vnr')
        for entry in [*result.get('screens',{}).values(),*result.get('spares',[])]:
            url=entry['url']
            if not re.fullmatch(frame_route, url): raise ValueError('Invalid frame path')
            path=root/url.lstrip('/')
            if path.resolve()!=path: raise ValueError('Invalid frame path')
            with path.open('rb') as f:
                digest = hashlib.file_digest(f, 'sha256').hexdigest()
            if profile is not None and (entry['id'] != digest[:16] or entry['sha256'] != digest):
                raise ValueError('LCD frame differs from its published hash')
            entry['sha256'] = digest
        result['sig'] = hmac.new(self.key if key is None else key, canonical(result), hashlib.sha256).hexdigest()
        return result
