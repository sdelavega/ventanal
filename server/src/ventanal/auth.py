# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Stephen de la Vega
"""Private password storage, signed sessions and bounded login attempts."""
import base64
import hashlib
import hmac
import json
import os
from pathlib import Path
import secrets
from threading import RLock
import time


def private_write(path, data):
    tmp = path.with_name(path.name + '.' + secrets.token_hex(8))
    try:
        fd = os.open(tmp, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        with os.fdopen(fd, 'wb') as f:
            f.write(data); f.flush(); os.fsync(f.fileno())
        os.replace(tmp, path)
    finally:
        tmp.unlink(missing_ok=True)


class Auth:
    def __init__(self, directory: Path):
        directory.mkdir(parents=True, exist_ok=True)
        self.path = directory / 'auth.json'
        secret = directory / 'session.secret'
        try:
            fd = os.open(secret, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        except FileExistsError:
            pass
        else:
            with os.fdopen(fd, 'wb') as f: f.write(secrets.token_bytes(32))
        os.chmod(secret, 0o600)
        self.secret = secret.read_bytes()
        self.state = json.loads(self.path.read_text()) if self.path.exists() else {'mode':'unset', 'epoch':secrets.token_hex(16)}
        self.lock = RLock()
        self.attempts = {}

    @property
    def mode(self): return self.state['mode']

    def _save(self): private_write(self.path, json.dumps(self.state).encode())

    def none(self):
        with self.lock:
            self.state['mode'] = 'none'; self._save()

    def invalidate(self):
        with self.lock:
            self.state['epoch'] = secrets.token_hex(16); self._save()

    def set_password(self, password, confirm):
        if not password or len(password) > 1024 or password != confirm:
            raise ValueError('Passwords must match and contain 1–1024 characters.')
        salt = secrets.token_bytes(16)
        digest = hashlib.scrypt(password.encode(), salt=salt, n=2**14, r=8, p=1).hex()
        with self.lock:
            self.state = {'mode':'password', 'salt':salt.hex(), 'hash':digest, 'epoch':secrets.token_hex(16)}
            self._save()

    def verify(self, password):
        with self.lock:
            if self.mode != 'password' or len(password) > 1024: return False
            digest = hashlib.scrypt(password.encode(), salt=bytes.fromhex(self.state['salt']), n=2**14, r=8, p=1).hex()
            return hmac.compare_digest(digest, self.state['hash'])

    def sign(self, session):
        body = base64.urlsafe_b64encode(json.dumps(session, separators=(',', ':')).encode()).decode()
        return body + '.' + hmac.new(self.secret, body.encode(), hashlib.sha256).hexdigest()

    def issue(self, granted):
        s = {'csrf':secrets.token_hex(32), 'exp':int(time.time())+30*86400, 'epoch':self.state['epoch'], 'granted':granted}
        return s, self.sign(s)

    def session(self, cookie):
        try:
            body, sig = cookie.split('.')
            if not hmac.compare_digest(sig, hmac.new(self.secret, body.encode(), hashlib.sha256).hexdigest()): return None
            s = json.loads(base64.urlsafe_b64decode(body))
            if s['exp'] <= time.time() or s['epoch'] != self.state['epoch']: return None
            return s
        except (ValueError, KeyError, TypeError): return None

    def allowed(self, ip, now=None):
        now = time.monotonic() if now is None else now
        with self.lock:
            entries = [t for t in self.attempts.get(ip, []) if t > now-60]
            self.attempts[ip] = entries
            return len(entries) < 10 and (not entries or now-entries[-1] >= min(2**(len(entries)-1), 30))

    def failed(self, ip, now=None):
        now = time.monotonic() if now is None else now
        with self.lock:
            self.attempts = {k:v for k,v in self.attempts.items() if v and v[-1] > now-60}
            self.attempts.setdefault(ip, []).append(now)
