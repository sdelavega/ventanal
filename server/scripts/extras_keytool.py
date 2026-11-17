# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Stephen de la Vega
"""Offline developer utility. Never prints private keys or tokens."""

import argparse
import base64
import json
import os
from pathlib import Path
from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey

REPO = Path(__file__).resolve().parents[2]


def external(path):
    path = Path(path).expanduser().resolve()
    if path.is_relative_to(REPO):
        raise ValueError("Private keys must be outside the repository")
    return path


def exclusive(path, data):
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    with os.fdopen(fd, "wb") as stream:
        stream.write(data)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    gen = sub.add_parser("generate")
    gen.add_argument("--private", required=True)
    gen.add_argument("--public", required=True)
    sign = sub.add_parser("sign")
    sign.add_argument("--private", required=True)
    sign.add_argument("--payload", required=True)
    sign.add_argument("--output", required=True)
    args = parser.parse_args()
    try:
        private = external(args.private)
        if args.command == "generate":
            public = Path(args.public).expanduser().resolve()
            if public == private or public.exists() or private.exists():
                raise ValueError("Output already exists or paths overlap")
            key = Ed25519PrivateKey.generate()
            exclusive(
                private,
                key.private_bytes(
                    serialization.Encoding.PEM,
                    serialization.PrivateFormat.PKCS8,
                    serialization.NoEncryption(),
                ),
            )
            exclusive(
                public,
                key.public_key().public_bytes(
                    serialization.Encoding.PEM,
                    serialization.PublicFormat.SubjectPublicKeyInfo,
                ),
            )
        else:
            key = serialization.load_pem_private_key(
                private.read_bytes(), password=None
            )
            if not isinstance(key, Ed25519PrivateKey):
                raise ValueError("Ed25519 key required")
            payload = json.loads(Path(args.payload).read_text())
            if not isinstance(payload, dict):
                raise ValueError("Payload must be a JSON object")
            data = json.dumps(
                payload,
                sort_keys=True,
                separators=(",", ":"),
                ensure_ascii=False,
                allow_nan=False,
            ).encode()
            enc = lambda b: base64.urlsafe_b64encode(b).rstrip(b"=")
            exclusive(Path(args.output), enc(data) + b"." + enc(key.sign(data)) + b"\n")
    except (OSError, ValueError, TypeError):
        parser.exit(
            1, "Key operation failed; check paths, permissions and input format.\n"
        )


if __name__ == "__main__":
    main()
