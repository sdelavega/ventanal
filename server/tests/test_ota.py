# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Stephen de la Vega
from datetime import datetime, timedelta, timezone
import hashlib
import hmac
from http.client import HTTPConnection, HTTPSConnection
import json
from pathlib import Path
import re
import ssl
from threading import Event, Thread, get_ident
from types import SimpleNamespace
from urllib.parse import urlencode

from cryptography.hazmat.primitives.serialization import load_pem_private_key
import httpx
import pytest

from test_security import proof
from ventanal.manifest import MAX_SERVED_MANIFEST_BYTES, encode_board_manifest
from ventanal.ota import FirmwareStore, OPEN_DEVICE, poll_github, verified_metadata
from ventanal.release import canonical_release_json, sign_release, ReleaseError
from ventanal.security import canonical
from ventanal.serve import make_server
from ventanal.status import parse_board

BOARD = "inkplate13spectra"
LCD = "lcd43b"
IMAGE = b"\x00\xff\x80\r\nfirmware\n"
IMAGE_PATH = f"/firmware/{BOARD}-2.0.0.bin"
CHECKIN = f"/manifest.json?board={BOARD}&fw=1.0.0"
EXAMPLE = Path(__file__).parents[1] / "ventanal.example.toml"


@pytest.fixture
def signed():
    key = load_pem_private_key(
        (Path(__file__).parent / "fixtures/release/release-test-only.pem").read_bytes(),
        password=None,
    )

    def sign(board=BOARD, version="2.0.0", image=IMAGE, minimum="0.0.0", notes=None):
        metadata = canonical_release_json({
            "board": board, "version": version, "size": len(image),
            "sha256": hashlib.sha256(image).hexdigest(), "min_from_version": minimum,
            "notes_url": notes or f"https://github.com/sdelavega/ventanal/releases/tag/v{version}",
        })
        return metadata, sign_release(metadata, key).encode("ascii")

    return sign


def test_poll_verifies_and_caches_only_usable_images(tmp_path, signed):
    store = FirmwareStore(tmp_path)
    store.checkin(BOARD, OPEN_DEVICE, "1.0.0", None)
    assets = {}
    listing = []

    def add(board, version, image=IMAGE, bad=None, minimum="0.0.0"):
        metadata, sig = signed(board, version, image, minimum)
        stem = f"ventanal-{board}-{version}"
        files = {stem + ".json": metadata, stem + ".json.sig": sig, stem + ".bin": image}
        if bad == "signature":
            files[stem + ".json.sig"] = b"A" * 86 + b"=="
        if bad == "hash":
            files[stem + ".bin"] = bytes(len(image))
        assets.update(files)
        listing.append({"tag_name": f"v{version}", "assets": [{"name": name} for name in files]})

    add(BOARD, "2.0.0")
    add(BOARD, "3.0.0", bad="signature")
    add(BOARD, "4.0.0", bad="hash")
    add(BOARD, "5.0.0", minimum="1.1.0")
    add(LCD, "6.0.0")
    requests = []

    def github(request):
        requests.append(request.url)
        assert request.headers["User-Agent"].startswith("Ventanal/")
        assert "Authorization" not in request.headers and "Cookie" not in request.headers
        if request.url.host == "api.github.com":
            assert request.url.path == "/repos/sdelavega/ventanal/releases"
            return httpx.Response(200, json=listing)
        name = request.url.path.split("/")[-1]
        if request.url.host == "github.com":
            return httpx.Response(302, headers={"Location": f"https://release-assets.githubusercontent.com/{name}"})
        assert request.url.host == "release-assets.githubusercontent.com"
        return httpx.Response(200, content=assets[name])

    now = datetime(2026, 11, 18, tzinfo=timezone.utc)
    with httpx.Client(transport=httpx.MockTransport(github)) as client:
        poll_github(store, client, OPEN_DEVICE, now=now)
        requests.clear()
        poll_github(store, client, OPEN_DEVICE, now=now + timedelta(hours=1))
    assert store.image(f"{BOARD}-2.0.0.bin") == IMAGE
    assert store.image(f"{BOARD}-3.0.0.bin") is None
    assert store.image(f"{BOARD}-4.0.0.bin") is None
    assert store.image(f"{BOARD}-5.0.0.bin") is None
    assert store.image(f"{LCD}-6.0.0.bin") is None
    assert not any(f"{version}.bin" in url.path for url in requests for version in
                   ("2.0.0", "3.0.0", "5.0.0", "6.0.0"))
    assert "BadSignature" in store.last_poll()["error"]
    assert "SHA-256" in store.last_poll()["error"]
    assert not store.poll_due(now + timedelta(hours=24))
    assert store.poll_due(now + timedelta(hours=25))


@pytest.mark.parametrize(("failure", "message"), [
    (403, "GitHub returned HTTP 403"),
    ("timeout", "GitHub request timed out"),
    ("malformed", "Malformed GitHub release list"),
])
def test_poll_failures_preserve_cached_images(tmp_path, signed, failure, message):
    store = FirmwareStore(tmp_path)
    store.cache(verified_metadata(*signed()), IMAGE, OPEN_DEVICE)

    def github(request):
        if failure == "timeout":
            raise httpx.ReadTimeout("details", request=request)
        if failure == "malformed":
            return httpx.Response(200, json=[{"tag_name": "v2.0.0", "assets": "bad"}])
        return httpx.Response(failure)

    with httpx.Client(transport=httpx.MockTransport(github)) as client:
        poll_github(store, client, OPEN_DEVICE)
    assert store.last_poll()["error"] == message
    assert store.image(f"{BOARD}-2.0.0.bin") == IMAGE


def test_device_approvals_are_isolated_and_survive_restart(tmp_path, signed):
    store = FirmwareStore(tmp_path)
    store.cache(verified_metadata(*signed()), IMAGE, "pair-A")
    store.cache(verified_metadata(*signed(LCD)), IMAGE, "pair-A")
    for board, device in ((BOARD, "pair-A"), (BOARD, "pair-B"), (LCD, "pair-A"), (BOARD, OPEN_DEVICE)):
        assert store.checkin(board, device, "1.0.0", None) is None
    assert not store.boards("pair-A")[0][0]["automatic"]
    store.approve(BOARD, "pair-A", "2.0.0")
    store = FirmwareStore(tmp_path)
    assert store.checkin(BOARD, "pair-B", "1.0.0", None) is None
    assert store.checkin(BOARD, OPEN_DEVICE, "1.0.0", None) is None
    assert store.checkin(LCD, "pair-A", "1.0.0", None) is None
    assert store.checkin(BOARD, "pair-A", "1.0.0", "failed:BadHash")["path"] == IMAGE_PATH
    assert store.checkin(BOARD, "pair-A", "1.0.0", "rolled_back:2.0.0") is None
    store.set_automatic(BOARD, "pair-A", True)
    assert store.checkin(BOARD, "pair-A", "1.0.0", None) is None
    store.cache(verified_metadata(*signed(version="3.0.0")), IMAGE, "pair-A")
    assert store.checkin(BOARD, "pair-A", "1.0.0", None)["path"].endswith("3.0.0.bin")
    assert store.checkin(BOARD, "pair-A", "3.0.0", "ok:3.0.0") is None
    state, releases = store.boards("pair-A")[0]
    assert state["running"] == "3.0.0" and state["result"] == "ok:3.0.0" and state["pending"] is None
    assert releases == []


def test_cache_prunes_old_images_and_retains_pending_and_last_upload(tmp_path, signed):
    store = FirmwareStore(tmp_path)
    store.checkin(BOARD, OPEN_DEVICE, "1.0.0", None)
    for major in range(2, 9):
        release = verified_metadata(*signed(version=f"{major}.0.0"))
        store.cache(release, IMAGE, OPEN_DEVICE, manual=(major == 3))
        if major == 2:
            store.approve(BOARD, OPEN_DEVICE, "2.0.0")
    for major in (2, 3, 6, 7, 8):
        assert store.image(f"{BOARD}-{major}.0.0.bin") == IMAGE
    for major in (4, 5):
        assert store.image(f"{BOARD}-{major}.0.0.bin") is None
    with pytest.raises(ReleaseError, match="already uses"):
        store.cache(verified_metadata(*signed(version="8.0.0", image=b"replacement")),
                    b"replacement", OPEN_DEVICE)
    assert store.image(f"{BOARD}-8.0.0.bin") == IMAGE


def test_checkin_validates_new_fields():
    assert parse_board({"board": [BOARD], "ota": ["rolled_back:2.0.0"]}) == {
        "board": BOARD, "ota": "rolled_back:2.0.0",
    }
    assert parse_board({"board": ["unknown"], "ota": ["ok:65536.0.0"]}) == {}
    assert parse_board({"board": [BOARD, LCD], "ota": ["failed:<script>"]}) == {}


@pytest.fixture
def web(tmp_path):
    path = tmp_path / "settings.toml"
    path.write_text(EXAMPLE.read_text().replace("/var/lib/ventanal", str(tmp_path))
                    .replace("/var/cache/ventanal", str(tmp_path / "cache")))
    (tmp_path / "manifest.json").write_text('{"version":1,"screens":{}}')
    server = make_server(tmp_path, "127.0.0.1", 0, cache_dir=tmp_path / "cache",
                         config_path=path, https_host="127.0.0.1", https_port=0)
    thread = Thread(target=server.serve_forever, daemon=True)
    thread.start()

    def request(path, *, data=None, headers=None, tls=False):
        if tls:
            context = ssl.create_default_context(cadata=server.ui.security.certificate)
            conn = HTTPSConnection(*server.tls_server.server_address, context=context, timeout=5)
        else:
            conn = HTTPConnection(*server.server_address, timeout=5)
        try:
            conn.request("GET" if data is None else "POST", path, data, headers or {})
            response = conn.getresponse()
            return response.status, response.read(), response.getheader("Set-Cookie")
        finally:
            conn.close()

    try:
        yield server, request, tmp_path
    finally:
        server.shutdown()
        server.server_close()
        thread.join()


def multipart(parts):
    chunks = []
    for name, data in parts.items():
        filename = f'; filename="{name}"' if name != "csrf" else ""
        headers = f'--test-boundary\r\nContent-Disposition: form-data; name="{name}"{filename}\r\n\r\n'
        chunks.extend((headers.encode(), data, b"\r\n"))
    chunks.append(b"--test-boundary--\r\n")
    return b"".join(chunks)


def test_open_status_upload_install_and_result(web, signed):
    server, request, _ = web
    server.ui.auth.none()
    assert "firmware" not in json.loads(request(CHECKIN)[1])
    assert request("/manifest.json?board=lcd43b&fw=1.0.0")[0] == 200
    _, page, cookie = request("/")
    csrf = re.search(rb'name="csrf" value="([^"]+)"', page)[1].decode()
    headers = {"Cookie": cookie.split(";")[0]}
    upload_headers = {**headers, "Content-Type": "multipart/form-data; boundary=test-boundary"}
    post_headers = {**headers, "Content-Type": "application/x-www-form-urlencoded"}
    metadata, sig = signed()
    parts = {"csrf": csrf.encode(), "metadata": metadata, "signature": sig, "image": IMAGE}
    assert request("/updates/upload", data=multipart({**parts, "csrf": b"x"}), headers=upload_headers)[0] == 403
    assert request("/updates/upload", data=multipart({**parts, "image": b"x"}), headers=upload_headers)[0] == 400
    assert request("/updates/upload", data=multipart({**parts, "signature": b"A"*86+b"=="}), headers=upload_headers)[0] == 400
    assert request(IMAGE_PATH)[0] == 404
    assert request("/updates/upload", data=multipart(parts), headers=upload_headers)[0] == 303
    assert request(IMAGE_PATH)[:2] == (200, IMAGE)
    page = request("/", headers=headers)[1]
    assert b"Update available: 2.0.0" in page and b"Release notes" in page
    assert not re.search(rb'name="automatic"[^>]*checked', page)
    assert "firmware" not in json.loads(request(CHECKIN)[1])
    values = {"board": BOARD, "csrf": csrf, "version": "2.0.0"}
    assert request("/updates/install", data=urlencode({**values, "csrf": "wrong"}), headers=post_headers)[0] == 403
    assert request("/updates/install", data=urlencode(values), headers=post_headers)[0] == 303
    assert json.loads(request(CHECKIN)[1])["firmware"] == {
        "release": metadata.decode(), "sig": sig.decode(), "path": IMAGE_PATH,
    }
    assert "firmware" not in json.loads(request("/manifest.json?board=lcd43b&fw=1.0.0")[1])
    assert "firmware" not in json.loads(request(CHECKIN + "&ota=rolled_back:2.0.0")[1])
    assert b"Last OTA result rolled_back:2.0.0" in request("/", headers=headers)[1]
    auto = {"board": BOARD, "csrf": csrf, "automatic": "yes"}
    assert request("/updates/automatic", data=urlencode(auto), headers=post_headers)[0] == 303
    assert re.search(rb'name="automatic"[^>]*checked', request("/", headers=headers)[1])
    assert "firmware" not in json.loads(request(CHECKIN)[1])


def test_signed_manifest_full_budget_and_firmware_transport(web, signed, monkeypatch):
    server, request, root = web
    server.ui.enable_security()
    server.ui.auth.set_password("test password", "test password")
    device = server.ui.firmware_device()
    server.ui.firmware.cache(verified_metadata(*signed(notes="https://example.test/" + "x" * 1700)), IMAGE, device)
    extras = ("extra-" + "x" * 42, "extra-" + "y" * 42)
    monkeypatch.setattr("ventanal.extras.registry.active", lambda *a, **kw: [SimpleNamespace(screens=extras)])
    monkeypatch.setattr("ventanal.solar.licence.verify", lambda *a: SimpleNamespace(ok=True))
    (root / "frames").mkdir()

    def entry(name):
        url = f"/frames/{name}-aaaaaaaaaaaaaaaa.vnf"
        (root / url.lstrip("/")).write_bytes(b"frame")
        return {"id": "a" * 16, "url": url, "size": 960016, "rendered": "2026-11-15T14:30:00-05:00",
                "readout": {"x": 1020, "y": 0, "w": 580, "h": 104, "style": "shade"},
                "credit": {"title": "𐀀" * 500}}

    screens = ("art", "weather", "solar", *extras)
    manifest = {"version": 1, "screens": {name: entry(name) for name in screens},
                "spares": [entry(f"spare{i}") for i in range(3)]}
    (root / "manifest.json").write_text(json.dumps(manifest))
    key = server.ui.security.key
    assert request(IMAGE_PATH)[0] == 403
    assert request(IMAGE_PATH, tls=True)[0] == 403
    assert request("/frames/art-aaaaaaaaaaaaaaaa.vnf", tls=True)[0] == 403
    assert request("/updates/check", data="csrf=x", tls=True)[0] == 303
    assert request(CHECKIN, headers=proof(key, CHECKIN, 1), tls=True)[0] == 200
    server.ui.firmware.approve(BOARD, device, "2.0.0")
    status, body, _ = request(CHECKIN, headers=proof(key, CHECKIN, 2), tls=True)
    assert status == 200 and len(body) <= MAX_SERVED_MANIFEST_BYTES
    document = json.loads(body)
    assert len(document["spares"]) == 3 and tuple(document["screens"]) == screens
    assert document["firmware"]["path"] == IMAGE_PATH
    assert document.pop("sig") == hmac.new(key, canonical(document), hashlib.sha256).hexdigest()
    assert "firmware" not in json.loads((root / "manifest.json").read_text())
    headers = proof(key, IMAGE_PATH, 3)
    assert request(IMAGE_PATH, headers=headers, tls=True)[:2] == (200, IMAGE)
    assert request(IMAGE_PATH, headers=headers, tls=True)[0] == 403
    unknown = f"/firmware/{BOARD}-80.0.0.bin"
    assert request(unknown, headers=proof(key, unknown, 4), tls=True)[0] == 404
    assert request(IMAGE_PATH + "/..", headers=proof(key, IMAGE_PATH + "/..", 5), tls=True)[0] == 404
    server.ui.security.repair()
    new_key = server.ui.security.key
    assert request(CHECKIN, headers=proof(key, CHECKIN, 6), tls=True)[0] == 403
    assert "firmware" not in json.loads(request(CHECKIN, headers=proof(new_key, CHECKIN, 1), tls=True)[1])


def test_manual_check_runs_in_background_and_displays_error(web, monkeypatch):
    server, request, _ = web
    server.ui.auth.none()
    entered, finish, done = Event(), Event(), Event()
    request_threads = []
    previous_start = server.ui.start_firmware_check

    def start(*, force):
        request_threads.append(get_ident())
        previous_start(force=force)

    def poll(store, client, device):
        try:
            assert get_ident() not in request_threads and device == OPEN_DEVICE
            entered.set()
            assert finish.wait(5)
            store.record_poll(datetime.now(timezone.utc), "GitHub returned HTTP 403")
        finally:
            done.set()

    monkeypatch.setattr(server.ui, "start_firmware_check", start)
    monkeypatch.setattr("ventanal.ui.poll_github", poll)
    _, page, cookie = request("/")
    csrf = re.search(rb'name="csrf" value="([^"]+)"', page)[1].decode()
    headers = {"Cookie": cookie.split(";")[0], "Content-Type": "application/x-www-form-urlencoded"}
    try:
        assert request("/updates/check", data="csrf=wrong", headers=headers)[0] == 403
        assert not entered.is_set()
        assert request("/updates/check", data=urlencode({"csrf": csrf}), headers=headers)[0] == 303
        assert entered.wait(2)
        assert b"Checking GitHub" in request("/", headers=headers)[1]
    finally:
        finish.set()
    assert done.wait(2)
    assert b"GitHub returned HTTP 403" in request("/", headers=headers)[1]


def test_oversize_offer_preserves_existing_manifest():
    document = {"version": 1, "screens": {}, "optional": "x" * 5500}
    firmware = {"release": "x" * 900}
    body = encode_board_manifest(document, firmware, sign=lambda m: {**m, "sig": "0" * 64})
    assert len(body) <= MAX_SERVED_MANIFEST_BYTES
    assert "firmware" not in json.loads(body)
