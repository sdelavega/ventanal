# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Stephen de la Vega
"""Offline, strictly validated Ed25519 entitlements and private state."""

import base64
from dataclasses import dataclass
import fcntl
import hashlib
from importlib.resources import files
import json
import logging
from pathlib import Path
import re
import time
from contextlib import contextmanager
from cryptography.exceptions import InvalidSignature
from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PublicKey
from ventanal.auth import private_write

MAX_TOKEN = 16384
ID = re.compile(r"[a-z][a-z0-9_-]{0,47}")
FEATURE = re.compile(r"[a-z][a-z0-9_.-]{0,95}")


def embedded_keys():
    """Only package keys are trusted. Test public keys live outside the package."""
    result = {}
    for path in files("ventanal.extras").joinpath("keys").iterdir():
        if path.name.endswith(".pub"):
            key = serialization.load_pem_public_key(path.read_bytes())
            if not isinstance(key, Ed25519PublicKey):
                raise ValueError("Invalid embedded key")
            result[path.name[:-4]] = key
    return result


@dataclass(frozen=True)
class Result:
    status: str
    payload: dict | None = None

    @property
    def entitled(self):
        return self.status in ("valid", "grace")


def decode(value):
    if not re.fullmatch(r"[A-Za-z0-9_-]+", value):
        raise ValueError()
    result = base64.urlsafe_b64decode(value + "=" * (-len(value) % 4))
    if base64.urlsafe_b64encode(result).rstrip(b"=").decode() != value:
        raise ValueError()
    return result


def unique(pairs):
    result = {}
    for k, v in pairs:
        if k in result:
            raise ValueError()
        result[k] = v
    return result


def verify(token, *, now=None):
    now = time.time() if now is None else now
    try:
        if not isinstance(token, str) or len(token) > MAX_TOKEN:
            raise ValueError()
        body, signature = token.split(".")
        raw, sig = decode(body), decode(signature)
        if len(sig) != 64:
            raise ValueError()
        p = json.loads(raw, object_pairs_hook=unique)
        required = {
            "v",
            "kid",
            "licence_id",
            "licensee",
            "features",
            "kind",
            "issued",
            "grace_days",
        }
        if not isinstance(p, dict) or p.get("kind") not in (
            "perpetual",
            "subscription",
        ):
            raise ValueError()
        if set(p) != required | ({"expires"} if p["kind"] == "subscription" else set()):
            raise ValueError()
        if type(p["v"]) is not int or p["v"] != 1:
            raise ValueError()
        for name in ("kid", "licence_id"):
            if not isinstance(p[name], str) or not re.fullmatch(
                r"[A-Za-z0-9_-]{1,96}", p[name]
            ):
                raise ValueError()
        if not isinstance(p["licensee"], str) or not re.fullmatch(
            r"[0-9a-f]{64}", p["licensee"]
        ):
            raise ValueError()
        fs = p["features"]
        if (
            not isinstance(fs, list)
            or not 1 <= len(fs) <= 64
            or any(not isinstance(f, str) or not FEATURE.fullmatch(f) for f in fs)
            or len(set(fs)) != len(fs)
        ):
            raise ValueError()
        for name in ("issued", "grace_days", *(["expires"] if "expires" in p else [])):
            if type(p[name]) is not int:
                raise ValueError()
        if not 0 <= p["issued"] <= 253402300799 or not 0 <= p["grace_days"] <= 365:
            raise ValueError()
        if "expires" in p and not p["issued"] < p["expires"] <= 253402300799:
            raise ValueError()
    except (ValueError, TypeError, UnicodeError, RecursionError):
        return Result("invalid payload")
    try:
        key = embedded_keys().get(p["kid"])
    except (ValueError, TypeError, OSError):
        return Result("invalid trust store")
    if key is None:
        return Result("unknown signer")
    try:
        key.verify(sig, raw)
    except InvalidSignature:
        return Result("invalid signature")
    if p["issued"] > now + 300:
        return Result("issued in future")
    # Expiry/grace boundaries are exact; 300 seconds of skew is for issuance only.
    if p["kind"] == "perpetual" or now <= p["expires"]:
        return Result("valid", p)
    if now <= p["expires"] + p["grace_days"] * 86400:
        return Result("grace", p)
    return Result("expired", p)


class Store:
    """State beside auth/security, never TOML. Cross-process serialized writes."""

    def __init__(self, directory):
        self.directory = Path(directory)
        self.directory.mkdir(parents=True, exist_ok=True)
        self.path = self.directory / "extras.json"

    @contextmanager
    def locked(self):
        with (self.directory / "extras.lock").open("a") as lock:
            fcntl.flock(lock, fcntl.LOCK_EX)
            yield

    def read(self):
        try:
            state = json.loads(self.path.read_text())
            if (
                not isinstance(state, dict)
                or not isinstance(state.get("tokens"), dict)
                or not isinstance(state.get("settings"), dict)
                or any(not isinstance(v, str) for v in state["tokens"].values())
            ):
                raise ValueError()
            for setting in state["settings"].values():
                if (
                    not isinstance(setting, dict)
                    or type(setting.get("enabled")) is not bool
                    or not isinstance(setting.get("values"), dict)
                    or any(type(v) not in (str, bool) for v in setting["values"].values())
                ):
                    raise ValueError()
            return state
        except FileNotFoundError:
            return {"tokens": {}, "settings": {}}
        except (ValueError, UnicodeError, OSError, RecursionError):
            logging.getLogger(__name__).warning("Invalid extras state; treating as empty")
            return {"tokens": {}, "settings": {}}

    def save(self, state):
        private_write(self.path, json.dumps(state).encode())

    def licences(self, *, now=None):
        return [
            (ident, verify(value, now=now))
            for ident, value in self.read()["tokens"].items()
        ]

    def add(self, token, *, now=None):
        token = token.strip()
        result = verify(token, now=now)
        if result.payload is None:
            raise ValueError(result.status)
        ident = hashlib.sha256(token.encode()).hexdigest()
        with self.locked():
            state = self.read()
            if len(state["tokens"]) >= 100 and ident not in state["tokens"]:
                raise ValueError("Licence limit reached")
            state["tokens"][ident] = token
            self.save(state)
        return ident

    def remove(self, ident):
        with self.locked():
            state = self.read()
            found = state["tokens"].pop(ident, None) is not None
            if found:
                self.save(state)
        return found

    def configure(self, extra, enabled, values):
        fields = {f.id: f for f in extra.settings}
        if set(values) - fields.keys():
            raise ValueError("Unknown setting")
        with self.locked():
            state = self.read()
            previous = state["settings"].get(extra.id, {}).get("values", {})
            cleaned = dict(previous)
            for name, value in values.items():
                field = fields[name]
                if field.kind == "bool":
                    if value not in (True, False, "yes", "no"):
                        raise ValueError("Expected yes or no")
                    value = value is True or value == "yes"
                elif not isinstance(value, str) or len(value) > 1024:
                    raise ValueError("Setting too long")
                cleaned[name] = value
            state["settings"][extra.id] = {"enabled": bool(enabled), "values": cleaned}
            self.save(state)
