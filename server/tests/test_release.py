# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Stephen de la Vega
import hashlib
import json
from pathlib import Path

import pytest
from cryptography.hazmat.primitives.serialization import load_pem_private_key, load_pem_public_key

from ventanal.cli import main
from ventanal.release import (
    RELEASE_PUBLIC_KEY, ReleaseError, canonical_release_json, check_release, sign_release,
)

FIXTURES = Path(__file__).parent / "fixtures" / "release"
VECTORS = json.loads((FIXTURES / "vectors.json").read_bytes())
PRIVATE_KEY = FIXTURES / "release-test-only.pem"
PUBLIC_KEY = FIXTURES / "release-test-only.pub"


@pytest.mark.parametrize("case", VECTORS, ids=[case["name"] for case in VECTORS])
def test_shared_release_verdicts(case):
    metadata = case["metadata"].encode("utf-8")
    args = (metadata, case["signature"], case["board"], case["running"],
            RELEASE_PUBLIC_KEY, case["slot_size"])
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
    assert public_key.public_bytes_raw() == RELEASE_PUBLIC_KEY.public_bytes_raw()
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
    main([*command, "verify", str(metadata), "--image", str(image),
          "--board", good["board"], "--running", good["running"],
          "--slot-size", str(good["slot_size"])])
    assert capsys.readouterr().out == "Verified inkplate13spectra 2.10.3\n"
    main([*command, "verify", str(metadata), "--key", str(PUBLIC_KEY)])
    image.write_bytes(b"text")
    with pytest.raises(SystemExit) as exc:
        main([*command, "verify", str(metadata), "--image", str(image)])
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
