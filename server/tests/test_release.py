# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Stephen de la Vega
import hashlib
import json
import os
from pathlib import Path
import re
import shutil

import pytest
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PublicKey
from cryptography.hazmat.primitives.serialization import (
    Encoding, PublicFormat, load_pem_private_key, load_pem_public_key,
)

from ventanal.cli import main
from ventanal.release import (
    TEST_RELEASE_PUBLIC_KEY_HEX, ReleaseError, canonical_release_json, check_release, sign_release,
)

FIXTURES = Path(__file__).parent / "fixtures" / "release"
VECTORS = json.loads((FIXTURES / "vectors.json").read_bytes())
PRIVATE_KEY = FIXTURES / "release-test-only.pem"
PUBLIC_KEY = FIXTURES / "release-test-only.pub"
TEST_KEY = load_pem_public_key(PUBLIC_KEY.read_bytes())


@pytest.mark.parametrize("case", VECTORS, ids=[case["name"] for case in VECTORS])
def test_shared_release_verdicts(case):
    metadata = case["metadata"].encode("utf-8")
    args = (metadata, case["signature"], case["board"], case["running"],
            TEST_KEY, case["slot_size"])
    if case["verdict"] == "Ok":
        info = check_release(*args)
        assert canonical_release_json(vars(info)) == metadata
        if "image" in case:
            image = case["image"].encode("utf-8")
            assert info.matches_image(len(image), hashlib.sha256(image).digest()) == case["image_matches"]
    else:
        with pytest.raises(ReleaseError) as exc:
            check_release(*args)
        assert exc.value.reason == case["verdict"]


def test_test_key_matches_release_signatures():
    private_key = load_pem_private_key(PRIVATE_KEY.read_bytes(), None)
    public_key = load_pem_public_key(PUBLIC_KEY.read_bytes())
    assert private_key.public_key().public_bytes_raw() == public_key.public_bytes_raw()
    assert public_key.public_bytes_raw().hex() == TEST_RELEASE_PUBLIC_KEY_HEX
    good = VECTORS[0]
    assert sign_release(good["metadata"].encode(), private_key) == good["signature"]
    with pytest.raises(ReleaseError, match="BadSignature"):
        sign_release(good["metadata"].encode() + b"\n", private_key)


def test_release_cli(tmp_path, capsys):
    good = VECTORS[0]
    metadata = tmp_path / "release.json"
    metadata.write_bytes(good["metadata"].encode())
    image = tmp_path / "release.bin"
    image.write_bytes(good["image"].encode())
    config = tmp_path / "unused.toml"
    command = ["--config", str(config), "release"]
    main([*command, "sign", str(metadata), "--key", str(PRIVATE_KEY)])
    assert metadata.with_suffix(".json.sig").read_text() == good["signature"]
    main([*command, "verify", str(metadata), "--image", str(image), "--key", str(PUBLIC_KEY),
          "--board", good["board"], "--running", good["running"],
          "--slot-size", str(good["slot_size"])])
    assert capsys.readouterr().out == "Verified inkplate13spectra 2.10.3\n"
    main([*command, "verify", str(metadata), "--key", str(PUBLIC_KEY)])
    image.write_bytes(b"text")
    with pytest.raises(SystemExit) as exc:
        main([*command, "verify", str(metadata), "--image", str(image), "--key", str(PUBLIC_KEY)])
    assert exc.value.code == 2
    assert "Image size or SHA-256 mismatch" in capsys.readouterr().err


def test_release_keygen_preserves_existing_public_key(tmp_path):
    private = tmp_path / "new.pem"
    public = tmp_path / "existing.pub"
    public.write_bytes(b"existing")
    with pytest.raises(SystemExit) as exc:
        main(["release", "keygen", str(private), str(public)])
    assert exc.value.code == 2
    assert not private.exists()
    assert public.read_bytes() == b"existing"


def test_release_keygen_cleans_up_when_fdopen_fails(tmp_path, monkeypatch):
    private = tmp_path / "new.pem"
    public = tmp_path / "new.pub"
    fdopen = os.fdopen
    descriptors = []

    def fail_second_open(fd, *args, **kwargs):
        descriptors.append(fd)
        if len(descriptors) == 2:
            raise RuntimeError("fdopen failed")
        return fdopen(fd, *args, **kwargs)

    monkeypatch.setattr(os, "fdopen", fail_second_open)
    with pytest.raises(RuntimeError, match="fdopen failed"):
        main(["release", "keygen", str(private), str(public)])
    assert not private.exists()
    assert not public.exists()
    assert len(descriptors) == 2
    for fd in descriptors:
        with pytest.raises(OSError):
            os.fstat(fd)


def test_install_public_key_writes_both_active_keys_and_keeps_fixture(tmp_path, monkeypatch):
    root = Path(__file__).resolve().parents[2]
    header_path = Path("firmware/common/release_key.h")
    server_path = Path("server/src/ventanal/release.py")
    for path in (header_path, server_path):
        (tmp_path / path.parent).mkdir(parents=True)
        shutil.copyfile(root / path, tmp_path / path)
    monkeypatch.chdir(tmp_path)
    old_server = server_path.read_text()
    # A deterministic public-only fixture. No owner keypair is generated.
    public_bytes = bytes(range(32))
    public = tmp_path / "synthetic.pub"
    public.write_bytes(Ed25519PublicKey.from_public_bytes(public_bytes).public_bytes(
        Encoding.PEM, PublicFormat.SubjectPublicKeyInfo,
    ))
    main(["release", "install-public-key", str(public)])
    header = header_path.read_text()
    active, fixture = header.split("inline constexpr uint8_t testKey[] = {", 1)
    assert ''.join(re.findall(r"0x([0-9a-f]{2})", active)) == public_bytes.hex()
    assert ''.join(re.findall(r"0x([0-9a-f]{2})", fixture)) == TEST_RELEASE_PUBLIC_KEY_HEX
    server = server_path.read_text()
    assert f'RELEASE_PUBLIC_KEY_HEX = "{public_bytes.hex()}"' in server
    assert f'TEST_RELEASE_PUBLIC_KEY_HEX = "{TEST_RELEASE_PUBLIC_KEY_HEX}"' in server
    server_path.write_text(old_server)  # Simulate interruption between the replacements.
    main(["release", "install-public-key", str(public)])
    assert server_path.read_text() == server
    with pytest.raises(SystemExit):
        main(["release", "install-public-key", str(PUBLIC_KEY)])
    assert header_path.read_text() == header and server_path.read_text() == server
