# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Stephen de la Vega
"""Throwaway signers and a test-only entry point exercise the extras boundary."""

import base64
import hashlib
import json
from dataclasses import replace
from datetime import datetime, timezone
from types import SimpleNamespace
import pytest
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey
from ventanal.extras.licence import verify, Store
from ventanal.extras.registry import discover, active, Manifest, Field
from test_gui import web

NOW = 1800000000


def token(key, **overrides):
    payload = dict(
        v=1,
        kid="test",
        licence_id="test-licence",
        licensee=hashlib.sha256(b"example@example.invalid").hexdigest(),
        features=["test.feature"],
        kind="subscription",
        issued=NOW - 100,
        expires=NOW + 100,
        grace_days=7,
    )
    payload.update(overrides)
    data = json.dumps(payload, separators=(",", ":")).encode()
    enc = lambda b: base64.urlsafe_b64encode(b).rstrip(b"=").decode()
    return enc(data) + "." + enc(key.sign(data))


@pytest.fixture
def signer(monkeypatch):
    key = Ed25519PrivateKey.generate()
    monkeypatch.setattr(
        "ventanal.extras.licence.embedded_keys", lambda: {"test": key.public_key()}
    )
    return key


def test_tokens(signer):
    good = token(signer)
    assert verify(good, now=NOW).status == "valid"
    assert verify(good, now=NOW + 101).status == "grace"
    assert verify(good, now=NOW + 8 * 86400).status == "expired"
    assert not verify(good, now=NOW + 8 * 86400).entitled
    assert (
        verify(token(signer, kind="perpetual", expires=None), now=NOW).status
        == "invalid payload"
    )
    payload = token(signer, kind="perpetual").split(".")[0]
    data = json.loads(base64.urlsafe_b64decode(payload + "=="))
    data.pop("expires")
    raw = json.dumps(data).encode()
    enc = lambda b: base64.urlsafe_b64encode(b).rstrip(b"=").decode()
    assert verify(enc(raw) + "." + enc(signer.sign(raw)), now=NOW + 99999999).entitled
    assert verify(token(signer, kid="other"), now=NOW).status == "unknown signer"
    assert (
        verify(token(Ed25519PrivateKey.generate()), now=NOW).status
        == "invalid signature"
    )
    body, signature = good.split(".")
    changed = ("A" if signature[0] != "A" else "B") + signature[1:]
    assert verify(body + "." + changed, now=NOW).status == "invalid signature"
    assert (
        verify(token(signer, issued=NOW + 301, expires=NOW + 1000), now=NOW).status
        == "issued in future"
    )
    assert verify(token(signer, issued=NOW + 299, expires=NOW + 1000), now=NOW).entitled
    for change in [
        dict(features="test.feature"),
        dict(v=True),
        dict(grace_days=-1),
        dict(licensee="email@invalid"),
        dict(expires=NOW - 200),
        dict(extra="x"),
    ]:
        assert verify(token(signer, **change), now=NOW).status == "invalid payload"
    assert not verify("x" * 20000, now=NOW).entitled


def example():
    return Manifest(
        "test-extra",
        "Test extra",
        "A test screen",
        "add-on",
        "test.feature",
        "",
        (Field("caption", "Caption", "text", "Example"),),
        ("extra-test",),
        lambda cfg, now, settings: {},
    )


def test_discovery_activation_store(tmp_path, signer, monkeypatch):
    ep = SimpleNamespace(name="test", load=lambda: example())
    monkeypatch.setattr("ventanal.extras.registry.entry_points", lambda **kw: [ep])
    assert discover()[0].id == "test-extra"
    store = Store(tmp_path)
    assert not active(discover(), store, now=NOW)
    ident = store.add(token(signer), now=NOW)
    assert store.path.stat().st_mode & 0o777 == 0o600
    assert not active(discover(), store, now=NOW)
    store.configure(example(), True, {"caption": "Hello"})
    assert active(discover(), store, now=NOW)[0].id == "test-extra"
    assert not active(discover(), store, now=NOW + 8 * 86400)
    with pytest.raises(ValueError):
        store.configure(example(), True, {"unknown": "value"})
    assert store.remove(ident)
    assert not active(discover(), store, now=NOW)
    bad = replace(example(), kind="hosted")
    monkeypatch.setattr(
        "ventanal.extras.registry.entry_points",
        lambda **kw: [SimpleNamespace(name="bad", load=lambda: bad)],
    )
    assert not discover()


def test_production_has_no_signer():
    from ventanal.extras.licence import embedded_keys

    assert embedded_keys() == {}


def test_page(web, signer, monkeypatch):
    r, srv, root = web
    srv.ui.auth.set_password("test password", "test password")
    assert r("/extras")[0] == 303
    _, _, csrf = r("/login")
    assert r("/login", {"csrf": csrf, "password": "test password"})[0] == 303
    status, body, csrf = r("/extras")
    assert status == 200 and "No extras installed yet" in body
    assert "Licences" in body and "Available" in body
    assert r("/extras/licences", {"token": token(signer)})[0] == 403
    assert r("/extras/licences", {"csrf": csrf, "token": "bad"})[0] == 400
    monkeypatch.setattr(
        "ventanal.extras.registry.entry_points",
        lambda **kw: [SimpleNamespace(name="test", load=lambda: example())],
    )
    value = token(
        signer,
        issued=int(datetime.now(timezone.utc).timestamp()) - 100,
        expires=int(datetime.now(timezone.utc).timestamp()) + 86400,
    )
    assert r("/extras/licences", {"csrf": csrf, "token": value})[0] == 303
    _, body, csrf = r("/extras")
    assert value not in body and "test.feature" in body and "subscription" in body
    assert (
        r(
            "/extras/configure",
            {"csrf": csrf, "id": "test-extra", "enabled": "yes", "caption": "Hello"},
        )[0]
        == 200
    )
    assert "Active" in r("/extras")[1]
    assert (
        r(
            "/extras/remove",
            {"csrf": csrf, "id": hashlib.sha256(value.encode()).hexdigest()},
        )[0]
        == 303
    )


def test_publish_signed(tmp_path, signer, monkeypatch):
    import numpy as np
    from PIL import Image
    from ventanal.extras.publish import tick_extras, filter_manifest
    from ventanal.manifest import Readout, publish, read_manifest
    from ventanal.frame import encode_frame
    from ventanal.security import Security, canonical
    import hmac

    cfg = SimpleNamespace(cache_dir=tmp_path / "state", output_dir=tmp_path / "out")
    cfg.cache_dir.mkdir()
    now = datetime.fromtimestamp(NOW, timezone.utc)
    frame = encode_frame(np.zeros((1200, 1600), dtype=np.uint8))
    preview = Image.new("RGB", (800, 600), "white")
    result = (frame, Readout(10, 10, 300, 100, "lcars"), preview)
    extra = replace(example(), renderer=lambda c, n, s: {"extra-test": result})
    monkeypatch.setattr(
        "ventanal.extras.registry.entry_points",
        lambda **kw: [SimpleNamespace(name="test", load=lambda: extra)],
    )
    store = Store(cfg.cache_dir)
    store.add(token(signer), now=NOW)
    store.configure(extra, True, {})
    for name in ("weather", "art", "solar"):
        publish(cfg.output_dir, name, frame, result[1], None, preview, now)
    tick_extras(cfg, now)
    manifest = filter_manifest(read_manifest(cfg.output_dir), cfg.cache_dir, now=NOW)
    assert manifest["screen_order"] == ["art", "weather", "solar", "extra-test"]
    sec = Security(cfg.cache_dir)
    sec.enable(["localhost"])
    signed = sec.manifest(manifest, cfg.output_dir)
    signature = signed.pop("sig")
    assert hmac.compare_digest(
        signature, hmac.new(sec.key, canonical(signed), "sha256").hexdigest()
    )
    assert (
        signed["screens"]["extra-test"]["sha256"] == hashlib.sha256(frame).hexdigest()
    )
    store.configure(extra, False, {})
    assert (
        "extra-test" not in filter_manifest(manifest, cfg.cache_dir, now=NOW)["screens"]
    )


def test_real_entry_point(tmp_path, signer, monkeypatch):
    from pathlib import Path
    from ventanal.extras.publish import tick_extras
    from ventanal.manifest import read_manifest
    from types import SimpleNamespace

    fixture = Path(__file__).parent / "fixtures" / "extras_plugin"
    monkeypatch.syspath_prepend(str(fixture))
    installed = discover()
    extra = next(m for m in installed if m.id == "test-extra")
    cfg = SimpleNamespace(cache_dir=tmp_path / "state", output_dir=tmp_path / "out")
    store = Store(cfg.cache_dir)
    store.add(token(signer), now=NOW)
    store.configure(extra, True, {})
    tick_extras(cfg, datetime.fromtimestamp(NOW, timezone.utc))
    m = read_manifest(cfg.output_dir)
    assert m["screen_order"] == ["extra-test"]
    assert (
        cfg.output_dir / m["screens"]["extra-test"]["url"].lstrip("/")
    ).stat().st_size == 960016


def test_upload_and_limits(web, signer):
    from http.client import HTTPConnection

    r, srv, root = web
    srv.ui.auth.none()
    _, _, csrf = r("/extras")
    session, cookie = srv.ui.auth.issue(True)
    value = token(
        signer,
        issued=int(datetime.now(timezone.utc).timestamp()) - 100,
        expires=int(datetime.now(timezone.utc).timestamp()) + 1000,
    )

    def upload(data, token_csrf=session["csrf"], oversize=False):
        body = (
            f'--boundary\r\nContent-Disposition: form-data; name="csrf"\r\n\r\n{token_csrf}\r\n--boundary\r\nContent-Disposition: form-data; name="token"; filename="key.txt"\r\nContent-Type: text/plain\r\n\r\n{data}\r\n--boundary--\r\n'
        ).encode()
        c = HTTPConnection(*srv.server_address)
        c.request(
            "POST",
            "/extras/upload",
            body,
            {
                "Cookie": "ventanal=" + cookie,
                "Content-Type": "multipart/form-data; boundary=boundary",
            },
        )
        response = c.getresponse()
        status = response.status
        result = response.read()
        c.close()
        return status, result

    assert upload(value, "wrong")[0] == 403
    assert upload(value)[0] == 303
    assert value.encode() not in r("/extras")[1].encode()
    assert upload("x" * 30000)[0] == 413
    assert upload("x" * 16400)[0] == 400
    assert not list(root.glob("*.licence"))


def test_keytool_boundaries(tmp_path):
    import subprocess, sys
    from pathlib import Path

    script = Path(__file__).parents[1] / "scripts" / "extras_keytool.py"
    private = tmp_path / "key.pem"
    public = tmp_path / "key.pub"
    result = subprocess.run(
        [
            sys.executable,
            str(script),
            "generate",
            "--private",
            str(private),
            "--public",
            str(public),
        ],
        capture_output=True,
    )
    assert result.returncode == 0 and result.stdout == b""
    assert private.stat().st_mode & 0o777 == 0o600
    result = subprocess.run(
        [
            sys.executable,
            str(script),
            "generate",
            "--private",
            str(script.parent / "forbidden.pem"),
            "--public",
            str(tmp_path / "public.pub"),
        ],
        capture_output=True,
    )
    assert result.returncode != 0 and not (script.parent / "forbidden.pem").exists()
    assert private.read_bytes() not in result.stdout + result.stderr


def test_manifest_rejects_traversal(tmp_path):
    from ventanal.manifest import publish, Readout
    from PIL import Image

    with pytest.raises(ValueError):
        publish(
            tmp_path,
            "extra-../../oops",
            b"",
            Readout(0, 0, 1, 1, "lcars"),
            None,
            Image.new("RGB", (1, 1)),
            datetime.now(timezone.utc),
        )


def test_committed_test_vector():
    """Public, deterministic throwaway vector; never a production trust anchor."""
    from pathlib import Path
    from cryptography.hazmat.primitives import serialization

    key = Ed25519PrivateKey.from_private_bytes(
        hashlib.sha256(b"Ventanal TEST ONLY extras fixture").digest()
    )
    fixtures = Path(__file__).parent / "fixtures"
    assert (
        fixtures / "extras-test-only.pub"
    ).read_bytes() == key.public_key().public_bytes(
        serialization.Encoding.PEM, serialization.PublicFormat.SubjectPublicKeyInfo
    )
    assert (fixtures / "extras-test-only.token").read_text().strip() == token(key)
    assert verify(token(key), now=NOW).status == "unknown signer"


def test_renderer_cannot_publish_invalid_readout(tmp_path, signer, monkeypatch):
    from ventanal.extras.publish import tick_extras
    from ventanal.manifest import read_manifest, Readout
    import numpy as np
    from PIL import Image
    from ventanal.frame import encode_frame

    extra = replace(
        example(),
        renderer=lambda *a: {
            "extra-test": (
                encode_frame(np.zeros((1200, 1600), dtype=np.uint8)),
                Readout(-1, 0, 10, 10, "lcars"),
                Image.new("RGB", (800, 600)),
            )
        },
    )
    monkeypatch.setattr(
        "ventanal.extras.registry.entry_points",
        lambda **kw: [SimpleNamespace(name="test", load=lambda: extra)],
    )
    cfg = SimpleNamespace(cache_dir=tmp_path / "state", output_dir=tmp_path / "out")
    store = Store(cfg.cache_dir)
    store.add(token(signer), now=NOW)
    store.configure(extra, True, {})
    tick_extras(cfg, datetime.fromtimestamp(NOW, timezone.utc))
    assert "extra-test" not in read_manifest(cfg.output_dir)["screens"]


def test_catalogue_and_hosted_privacy(tmp_path, monkeypatch):
    from pathlib import Path
    from ventanal.extras import registry
    from ventanal.extras.ui import body

    entries = json.loads(
        (
            Path(__file__).parent / "fixtures" / "extras-catalogue-example.json"
        ).read_text()
    )
    valid = {**entries[0], "example": False, "id": "hosted-test", "name": "Hosted test"}
    entries += [
        valid,
        {**valid, "checkout_url": "https://buy.paddle.com.evil.invalid/x"},
        {**valid, "privacy": ""},
        {**valid, "checkout_url": "javascript:alert(1)"},
    ]
    (tmp_path / "catalogue.json").write_text(json.dumps(entries))
    monkeypatch.setattr(registry, "files", lambda *a: tmp_path)
    assert registry.catalogue() == [valid]
    extra = replace(
        example(),
        kind="hosted",
        privacy="Sends the selected station only when enabled.",
    )
    monkeypatch.setattr(
        registry,
        "entry_points",
        lambda **kw: [SimpleNamespace(name="hosted", load=lambda: extra)],
    )
    page = body(Store(tmp_path), "csrf")
    assert extra.privacy in page and 'target="_blank" rel="noopener noreferrer"' in page


def server_manifest_vector(root, monkeypatch):
    """The same publication fixture is consumed by the C++ transport suite."""
    from pathlib import Path
    from ventanal.extras.publish import tick_extras
    from ventanal.manifest import publish, read_manifest

    monkeypatch.syspath_prepend(
        str(Path(__file__).parent / "fixtures" / "extras_plugin")
    )
    from ventanal_test_extra import manifest

    key = Ed25519PrivateKey.generate()
    monkeypatch.setattr(
        "ventanal.extras.licence.embedded_keys", lambda: {"test": key.public_key()}
    )
    cfg = SimpleNamespace(cache_dir=root / "state", output_dir=root / "out")
    store = Store(cfg.cache_dir)
    store.add(token(key), now=NOW)
    store.configure(manifest, True, {})
    now = datetime.fromtimestamp(NOW, timezone.utc)
    frame, readout, preview = manifest.renderer(cfg, now, {})["extra-test"]
    for name in ("art", "weather", "solar"):
        publish(cfg.output_dir, name, frame, readout, None, preview, now)
    tick_extras(cfg, now)
    m = read_manifest(cfg.output_dir)
    # The vector is what a board without zlib support is served.
    from ventanal.manifest import select_epaper_encoding
    select_epaper_encoding(m, {})
    for entry in m["screens"].values():
        entry["sha256"] = hashlib.sha256(frame).hexdigest()
    return m


def test_cross_language_manifest_vector(tmp_path, monkeypatch):
    from pathlib import Path

    vector = (
        Path(__file__).parents[2]
        / "firmware"
        / "test"
        / "fixtures"
        / "extras-server-manifest.json"
    )
    assert json.loads(vector.read_text()) == server_manifest_vector(
        tmp_path, monkeypatch
    )


@pytest.mark.parametrize('data', ['{', 'null', '[]', '{}', '{"tokens":[],"settings":{}}', '{"tokens":{},"settings":{"x":null}}', '\udcff'])
def test_corrupt_store_is_empty(tmp_path, monkeypatch, caplog, data):
    from ventanal.extras.publish import tick_extras

    store = Store(tmp_path)
    store.path.write_bytes(data.encode('utf-8', errors='surrogateescape'))
    assert store.read() == {'tokens': {}, 'settings': {}}
    assert 'Invalid extras state; treating as empty' in caplog.text
    monkeypatch.setattr('ventanal.extras.registry.entry_points', lambda **kw: [])
    tick_extras(SimpleNamespace(cache_dir=tmp_path, output_dir=tmp_path / 'out'), datetime.fromtimestamp(NOW, timezone.utc))


def test_invalid_trust_store_returns_status(tmp_path, monkeypatch):
    from ventanal.extras import licence

    (tmp_path / 'keys').mkdir()
    (tmp_path / 'keys' / 'broken.pub').write_text('broken public key')
    monkeypatch.setattr(licence, 'files', lambda *a: tmp_path)
    result = verify(token(Ed25519PrivateKey.generate()), now=NOW)
    assert result.status == 'invalid trust store' and not result.entitled


def test_catalogue_skips_malformed_entries(tmp_path, monkeypatch):
    from ventanal.extras import registry

    valid = dict(id='test-extra', name='Test', description='Description', kind='add-on', checkout_url='https://buy.paddle.com/test')
    entries = [None, 3, [], 'bad', {}, valid]
    for field in ('id', 'name', 'description', 'kind', 'checkout_url'):
        entries += [{k: v for k, v in valid.items() if k != field}, {**valid, field: []}]
    (tmp_path / 'catalogue.json').write_text(json.dumps(entries))
    monkeypatch.setattr(registry, 'files', lambda *a: tmp_path)
    assert registry.catalogue() == [valid]


def test_render_reads_licences_once_and_escapes_ids(tmp_path, monkeypatch):
    from ventanal.extras.ui import body
    from ventanal.extras.licence import Result
    from unittest.mock import Mock

    store = Store(tmp_path)
    store.licences = Mock(return_value=[('\"<unsafe>', Result('unknown signer'))])
    page = body(store, 'csrf')
    store.licences.assert_called_once_with()
    assert '&quot;&lt;unsafe&gt;' in page and 'value="\"<unsafe>' not in page
    assert '/static/extras.js?v=' in page


def test_extras_navigation(web):
    r, srv, _ = web
    srv.ui.auth.none()
    assert 'class="extras cur" href="/extras">EXTRAS</a>' in r('/extras')[1]
    assert 'class="extras" href="/extras">EXTRAS</a>' in r('/')[1]


def test_keytool_sign(tmp_path, monkeypatch):
    import subprocess, sys
    from pathlib import Path
    from cryptography.hazmat.primitives import serialization

    script = Path(__file__).parents[1] / 'scripts' / 'extras_keytool.py'
    key = Ed25519PrivateKey.generate()
    private = tmp_path / 'key.pem'
    private.write_bytes(key.private_bytes(serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8, serialization.NoEncryption()))
    payload = tmp_path / 'payload.json'
    output = tmp_path / 'token.txt'
    raw = token(key).split('.')[0]
    payload.write_bytes(base64.urlsafe_b64decode(raw + '=='))
    args = [sys.executable, str(script), 'sign', '--private', str(private), '--payload', str(payload), '--output', str(output)]
    result = subprocess.run(args, capture_output=True)
    assert result.returncode == 0 and result.stdout == b''
    monkeypatch.setattr('ventanal.extras.licence.embedded_keys', lambda: {'test': key.public_key()})
    assert verify(output.read_text().strip(), now=NOW).entitled
    assert output.stat().st_mode & 0o777 == 0o600
    saved = output.read_bytes()
    assert subprocess.run(args, capture_output=True).returncode != 0
    assert output.read_bytes() == saved
    output.unlink()
    for value in (3, [], None, 'text'):
        payload.write_text(json.dumps(value))
        result = subprocess.run(args, capture_output=True)
        assert result.returncode != 0 and not output.exists()
        assert result.stdout == b'' and b'Traceback' not in result.stderr
