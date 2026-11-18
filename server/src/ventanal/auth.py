# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Stephen de la Vega
"""Private password storage, signed sessions and bounded login attempts."""
import base64
import fcntl
import hashlib
import hmac
import json
import os
from pathlib import Path
import secrets
from threading import RLock
import time


PASSWORD_MIN_LENGTH = 10
PASSWORD_MAX_LENGTH = 1024


def validate_password(password, confirm):
    if not PASSWORD_MIN_LENGTH <= len(password) <= PASSWORD_MAX_LENGTH or password != confirm:
        raise ValueError(f'Passwords must match and contain {PASSWORD_MIN_LENGTH}–{PASSWORD_MAX_LENGTH} characters.')


def private_write(path, data, *, replace=True, durable=False):
    tmp = path.with_name(path.name + '.' + secrets.token_hex(8))
    try:
        fd = os.open(tmp, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        with os.fdopen(fd, 'wb') as f:
            f.write(data); f.flush(); os.fsync(f.fileno())
        if replace:
            os.replace(tmp, path)
        else:
            os.link(tmp, path)
        if durable:
            fd = os.open(path.parent, os.O_RDONLY | os.O_DIRECTORY)
            try:
                os.fsync(fd)
            finally:
                os.close(fd)
    finally:
        tmp.unlink(missing_ok=True)


class Auth:
    def __init__(self, directory: Path):
        directory.mkdir(parents=True, exist_ok=True)
        self.path = directory / 'auth.json'
        self.token_path = directory / 'setup.token'
        secret = directory / 'session.secret'
        if not secret.exists():
            try:
                private_write(secret, secrets.token_bytes(32), replace=False)
            except FileExistsError:
                pass  # The service or setup-token command initialized it concurrently.
        os.chmod(secret, 0o600)
        self.secret = secret.read_bytes()
        if len(self.secret) != 32:
            raise ValueError('Invalid session secret. Expected 32 bytes.')
        self.state = json.loads(self.path.read_text()) if self.path.exists() else {'mode':'unset', 'epoch':secrets.token_hex(16)}
        self.lock = RLock()
        self.attempts = {}

    @property
    def mode(self): return self.state['mode']

    def issue_setup_token(self):
        with self.token_path.with_suffix('.lock').open('a') as lock:
            fcntl.flock(lock, fcntl.LOCK_EX)
            try:
                mode = json.loads(self.path.read_text())['mode']
            except FileNotFoundError:
                mode = 'unset'
            if mode not in ('unset', 'none'):
                return None
            token = secrets.token_hex(32)
            digest = hashlib.sha256(token.encode()).hexdigest()
            private_write(self.token_path, f'{mode}:{digest}'.encode(), durable=True)
            return token

    def consume_setup_token(self, token):
        with self.token_path.with_suffix('.lock').open('a') as lock:
            fcntl.flock(lock, fcntl.LOCK_EX)
            try:
                expected = self.token_path.read_bytes()
            except FileNotFoundError:
                return False
            digest = hashlib.sha256(token.encode()).hexdigest()
            if not hmac.compare_digest(expected, f'{self.mode}:{digest}'.encode()):
                return False
            private_write(self.token_path, b'', durable=True)
            return True

    def _save(self, state):
        private_write(self.path, json.dumps(state).encode())
        self.state = state

    def none(self):
        with self.lock:
            self._save({'mode': 'none', 'epoch': secrets.token_hex(16)})

    def invalidate(self):
        with self.lock:
            self._save({**self.state, 'epoch': secrets.token_hex(16)})

    def set_password(self, password, confirm):
        validate_password(password, confirm)
        salt = secrets.token_bytes(16)
        digest = hashlib.scrypt(password.encode(), salt=salt, n=2**14, r=8, p=1).hex()
        with self.lock:
            self._save({'mode':'password', 'salt':salt.hex(), 'hash':digest, 'epoch':secrets.token_hex(16)})

    def verify(self, password):
        with self.lock:
            if self.mode != 'password' or len(password) > PASSWORD_MAX_LENGTH: return False
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
