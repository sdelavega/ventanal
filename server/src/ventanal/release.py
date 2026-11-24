# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Stephen de la Vega
"""Canonical metadata and Ed25519 verification for firmware releases."""

import base64
from dataclasses import dataclass
import json
import os
import re

from cryptography.exceptions import InvalidSignature
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PublicKey

BOARDS = frozenset({"inkplate13spectra", "lcd43b"})
FIELDS = {"board", "version", "size", "sha256", "min_from_version", "notes_url"}
VERSION = re.compile(r"(?:0|[1-9][0-9]{0,4})(?:\.(?:0|[1-9][0-9]{0,4})){2}")
SHA256 = re.compile(r"[0-9a-f]{64}")
SIGNATURE = re.compile(r"[A-Za-z0-9+/]{86}==")

# Active key. Install together with the firmware release public key.
RELEASE_PUBLIC_KEY_HEX = "7596df915a6f10164ac7e0067a92453fad1f00c7bf1fc0c3c1c3a75382d38d52"
TEST_RELEASE_PUBLIC_KEY_HEX = "8e60d1c05ba577843e1df225fae9a39351ea69de2d53c8c6b70dbd89167fd3ed"
RELEASE_PUBLIC_KEY = Ed25519PublicKey.from_public_bytes(bytes.fromhex(RELEASE_PUBLIC_KEY_HEX))
TEST_RELEASE_KEY_ACTIVE = RELEASE_PUBLIC_KEY_HEX == TEST_RELEASE_PUBLIC_KEY_HEX
TEST_RELEASE_KEY_WARNING = "TEST RELEASE KEY"


def firmware_updates_enabled():
    return not TEST_RELEASE_KEY_ACTIVE or os.getenv("VENTANAL_ALLOW_TEST_KEY") == "1"


class ReleaseError(ValueError):
    def __init__(self, reason: str):
        self.reason = reason
        super().__init__(reason)


@dataclass(frozen=True)
class ReleaseInfo:
    board: str
    version: str
    size: int
    sha256: str
    min_from_version: str
    notes_url: str

    def matches_image(self, size: int, sha256: bytes) -> bool:
        """Compare the size and digest of a streamed image to signed metadata."""
        return size == self.size and sha256 == bytes.fromhex(self.sha256)


def version_tuple(value: str) -> tuple[int, int, int]:
    if not isinstance(value, str) or not VERSION.fullmatch(value):
        raise ValueError("Invalid firmware version")
    major, minor, patch = map(int, value.split("."))
    if max(major, minor, patch) > 65535:
        raise ValueError("Firmware version component exceeds 65535")
    return major, minor, patch


def canonical_release_json(info: dict) -> bytes:
    """Validate the six fields and encode the exact bytes to sign."""
    if not isinstance(info, dict) or set(info) != FIELDS:
        raise ValueError("Unexpected release fields")
    if not isinstance(info["board"], str) or info["board"] not in BOARDS:
        raise ValueError("Unknown release board")
    version_tuple(info["version"])
    version_tuple(info["min_from_version"])
    if type(info["size"]) is not int or not 0 <= info["size"] <= 0xFFFFFFFF:
        raise ValueError("Invalid release size")
    if not isinstance(info["sha256"], str) or not SHA256.fullmatch(info["sha256"]):
        raise ValueError("Invalid SHA-256 digest")
    if not isinstance(info["notes_url"], str):
        raise ValueError("Invalid release notes URL")
    return json.dumps(info, sort_keys=True, separators=(",", ":"), ensure_ascii=False).encode("utf-8")


def parse_release(json_bytes: bytes) -> ReleaseInfo:
    """Accept only the canonical representation, including exact field order."""
    try:
        info = json.loads(json_bytes.decode("utf-8"))
        if canonical_release_json(info) != json_bytes:
            raise ValueError("Noncanonical release metadata")
        return ReleaseInfo(**info)
    except (ValueError, RecursionError) as exc:
        raise ReleaseError("BadSignature") from exc


def sign_release(json_bytes, private_key) -> str:
    parse_release(json_bytes)
    return base64.b64encode(private_key.sign(json_bytes)).decode("ascii")


def verify_release(json_bytes, sig_b64, public_key) -> ReleaseInfo:
    """Verify the exact signed bytes and parse their metadata."""
    try:
        if not isinstance(sig_b64, str) or not SIGNATURE.fullmatch(sig_b64):
            raise ValueError("Invalid signature encoding")
        sig = base64.b64decode(sig_b64, validate=True)
        if base64.b64encode(sig).decode("ascii") != sig_b64:
            raise ValueError("Noncanonical signature encoding")
        public_key.verify(sig, json_bytes)
    except (ValueError, InvalidSignature) as exc:
        raise ReleaseError("BadSignature") from exc
    return parse_release(json_bytes)


def check_release(json_bytes, sig_b64, board, running, public_key, slot_size) -> ReleaseInfo:
    info = verify_release(json_bytes, sig_b64, public_key)
    if info.board != board:
        raise ReleaseError("WrongBoard")
    check_update(info, running, slot_size)
    return info


def check_update(info: ReleaseInfo, running: str, slot_size: int) -> None:
    """Apply install policy to already verified metadata."""
    try:
        running_version = version_tuple(running)
    except ValueError as exc:
        raise ReleaseError("NotNewer") from exc
    if version_tuple(info.version) <= running_version:
        raise ReleaseError("NotNewer")
    if running_version < version_tuple(info.min_from_version):
        raise ReleaseError("TooOld")
    if not 0 < info.size <= slot_size:
        raise ReleaseError("BadSize")
