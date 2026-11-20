# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Stephen de la Vega
"""Verified firmware cache, per-device approvals, and GitHub release checks."""

from contextlib import ExitStack, closing, contextmanager
from dataclasses import dataclass
from datetime import datetime, timedelta, timezone
import fcntl
import hashlib
from importlib.metadata import version
import json
import logging
import os
from pathlib import Path
import re
import secrets
import sqlite3
from tempfile import NamedTemporaryFile
from time import monotonic
from urllib.parse import urljoin, urlsplit

import httpx

from ventanal.release import (
    BOARDS, VERSION, RELEASE_PUBLIC_KEY, ReleaseError, ReleaseInfo, check_update,
    SHA256, TEST_RELEASE_KEY_WARNING, firmware_updates_enabled, parse_release,
    verify_release, version_tuple,
)

OPEN_DEVICE = "open"
SLOT_SIZE = 4 * 1024 * 1024
MAX_METADATA = 2048
RETAIN_RELEASES = 3
POLL_INTERVAL = timedelta(days=1)
DEVICE_RETENTION = timedelta(days=30)
GITHUB_REPO = "https://github.com/sdelavega/ventanal"
GITHUB_RELEASES = "https://api.github.com/repos/sdelavega/ventanal/releases"
ASSET_HOSTS = {"release-assets.githubusercontent.com", "objects.githubusercontent.com"}
IMAGE_NAME = re.compile(rf"({'|'.join(sorted(BOARDS))})-({VERSION.pattern})\.bin")
log = logging.getLogger(__name__)


@dataclass(frozen=True)
class CachedRelease:
    info: ReleaseInfo
    metadata: str
    signature: str
    uploaded: str | None = None

    def supports(self, running):
        try:
            check_update(self.info, running, SLOT_SIZE)
        except ReleaseError:
            return False
        return True

    def manifest_entry(self, approval):
        entry = {
            "release": self.metadata,
            "sig": self.signature,
            "path": f"/firmware/{self.info.board}-{self.info.version}.bin",
        }
        if approval:
            entry["approval"] = approval
        return entry


def verified_metadata(metadata, signature):
    if not firmware_updates_enabled():
        raise ReleaseError(f"{TEST_RELEASE_KEY_WARNING}: firmware updates are disabled")
    if len(metadata) > MAX_METADATA:
        raise ReleaseError("Release metadata exceeds the size limit")
    try:
        signature = signature.decode("ascii")
    except UnicodeError as exc:
        raise ReleaseError("BadSignature") from exc
    info = verify_release(metadata, signature, RELEASE_PUBLIC_KEY)
    if not 0 < info.size <= SLOT_SIZE:
        raise ReleaseError("BadSize")
    return CachedRelease(info, metadata.decode("utf-8"), signature)


class FirmwareStore:
    def __init__(self, cache_dir):
        cache_dir.mkdir(parents=True, exist_ok=True)
        self.path = cache_dir / "firmware.sqlite3"
        self.images = cache_dir / "firmware"
        with closing(self.connect()) as db, db:
            db.executescript("""
                CREATE TABLE IF NOT EXISTS firmware_releases (
                    board TEXT NOT NULL, version TEXT NOT NULL, metadata TEXT NOT NULL,
                    signature TEXT NOT NULL, image BLOB NOT NULL, uploaded TEXT,
                    PRIMARY KEY (board, version));
                CREATE TABLE IF NOT EXISTS firmware_devices (
                    board TEXT NOT NULL, device TEXT NOT NULL, running TEXT,
                    result TEXT, automatic INTEGER NOT NULL DEFAULT 0, pending TEXT,
                    pending_auto INTEGER NOT NULL DEFAULT 0, rolled_back TEXT,
                    PRIMARY KEY (board, device));
                CREATE TABLE IF NOT EXISTS firmware_poll (
                    id INTEGER PRIMARY KEY CHECK (id = 1), checked TEXT NOT NULL,
                    error TEXT NOT NULL);""")
            db.execute("BEGIN IMMEDIATE")
            columns = {column["name"] for column in db.execute("PRAGMA table_info(firmware_devices)")}
            if "approval" not in columns:
                db.execute("ALTER TABLE firmware_devices ADD COLUMN approval TEXT")
            if "last_seen" not in columns:
                db.execute("ALTER TABLE firmware_devices ADD COLUMN last_seen TEXT")
                db.execute("UPDATE firmware_devices SET last_seen=?", (datetime.now(timezone.utc).isoformat(),))

    def connect(self):
        db = sqlite3.connect(self.path, timeout=1)
        db.row_factory = sqlite3.Row
        return db

    @staticmethod
    def _releases(db, board):
        if not firmware_updates_enabled():
            return []
        releases = []
        for row in db.execute(
            "SELECT version, metadata, signature, uploaded FROM firmware_releases WHERE board=?", (board,)
        ):
            try:
                info = verify_release(row["metadata"].encode(), row["signature"], RELEASE_PUBLIC_KEY)
            except ReleaseError:
                continue
            if (info.board, info.version) == (board, row["version"]):
                releases.append(CachedRelease(info, row["metadata"], row["signature"], row["uploaded"]))
        return sorted(releases, key=lambda release: version_tuple(release.info.version), reverse=True)

    @staticmethod
    def _prune(db):
        for board in BOARDS:
            releases = FirmwareStore._releases(db, board)
            keep = {release.info.version for release in releases[:RETAIN_RELEASES]}
            manual = db.execute(
                "SELECT version FROM firmware_releases WHERE board=? AND uploaded IS NOT NULL ORDER BY uploaded DESC LIMIT 1",
                (board,),
            ).fetchone()
            if manual:
                keep.add(manual["version"])
            for state in db.execute(
                "SELECT running, pending FROM firmware_devices WHERE board=?", (board,)
            ):
                keep.add(state["pending"])
                best = next((release for release in releases if release.supports(state["running"])), None)
                if best:
                    keep.add(best.info.version)
            db.executemany(
                "DELETE FROM firmware_releases WHERE board=? AND version=?",
                [(board, release.info.version) for release in releases if release.info.version not in keep],
            )

    def cache(self, release, image, *, manual=False):
        info = verified_metadata(release.metadata.encode(), release.signature.encode()).info
        if not info.matches_image(len(image), hashlib.sha256(image).digest()):
            raise ReleaseError("Image size or SHA-256 does not match signed metadata")
        uploaded = datetime.now(timezone.utc).isoformat() if manual else None
        with closing(self.connect()) as db, db:
            db.execute("BEGIN IMMEDIATE")
            row = db.execute(
                "SELECT metadata, signature FROM firmware_releases WHERE board=? AND version=?",
                (info.board, info.version),
            ).fetchone()
            if row and (row["metadata"], row["signature"]) != (release.metadata, release.signature):
                try:
                    verify_release(row["metadata"].encode(), row["signature"], RELEASE_PUBLIC_KEY)
                except ReleaseError:
                    db.execute("DELETE FROM firmware_releases WHERE board=? AND version=?", (info.board, info.version))
                    row = None
                else:
                    raise ReleaseError("A different signed release already uses that board and version")
            if row is None:
                db.execute(
                    "INSERT INTO firmware_releases VALUES (?,?,?,?,?,?)",
                    (info.board, info.version, release.metadata, release.signature, image, uploaded),
                )
            elif manual:
                db.execute(
                    "UPDATE firmware_releases SET uploaded=? WHERE board=? AND version=?",
                    (uploaded, info.board, info.version),
                )
            self._prune(db)

    def retain_devices(self, active_devices, *, now=None):
        with closing(self.connect()) as db, db:
            db.execute("BEGIN IMMEDIATE")
            if active_devices:
                known = {row["device"] for row in db.execute("SELECT DISTINCT device FROM firmware_devices")}
                removed = sum(
                    db.execute("DELETE FROM firmware_devices WHERE device=?", (device,)).rowcount
                    for device in known.difference(active_devices)
                )
            else:
                cutoff = (now or datetime.now(timezone.utc)) - DEVICE_RETENTION
                removed = db.execute(
                    "DELETE FROM firmware_devices WHERE last_seen < ?", (cutoff.isoformat(),)
                ).rowcount
            if removed:
                self._prune(db)

    def cached_metadata(self, release):
        with closing(self.connect()) as db:
            return db.execute(
                "SELECT 1 FROM firmware_releases WHERE board=? AND version=? AND metadata=? AND signature=?",
                (release.info.board, release.info.version, release.metadata, release.signature),
            ).fetchone() is not None

    def image(self, name):
        """Compatibility shim for tests that expect the whole image as bytes."""
        with self.open_image(name) as image:
            if image is not None:
                source, _ = image
                return source.read()
        return None

    @contextmanager
    def open_image(self, name):
        """Yield a verified, seekable image and its signed SHA-256."""
        match = IMAGE_NAME.fullmatch(name)
        if match is None or not firmware_updates_enabled():
            yield None
            return
        self.images.mkdir(exist_ok=True, mode=0o700)
        with ExitStack() as files:
            with (self.images / ".lock").open("a") as lock:
                # Coordinate pruning and cold cache fills across server processes.
                fcntl.flock(lock, fcntl.LOCK_EX)
                with closing(self.connect()) as db:
                    db.execute("BEGIN")
                    self._prune_images(db)
                    image = self._open_cached_image(db, match.groups(), files)
            # The open file survives pruning while a slow client is downloading.
            yield image

    def _prune_images(self, db):
        cached = {path for path in self.images.glob("*.bin") if SHA256.fullmatch(path.stem)}
        if not cached:
            return
        for row in db.execute("SELECT metadata FROM firmware_releases"):
            try:
                info = parse_release(row["metadata"].encode())
            except ReleaseError:
                continue
            cached.discard(self.images / f"{info.sha256}.bin")
        for path in cached:
            path.unlink(missing_ok=True)

    def _open_cached_image(self, db, release, files):
        row = db.execute(
            "SELECT rowid, metadata, signature FROM firmware_releases WHERE board=? AND version=?", release
        ).fetchone()
        if row is None:
            return None
        try:
            info = verify_release(row["metadata"].encode(), row["signature"], RELEASE_PUBLIC_KEY)
        except ReleaseError:
            return None
        if (info.board, info.version) != release:
            return None
        path = self.images / f"{info.sha256}.bin"
        try:
            if path.stat().st_size == info.size:
                return files.enter_context(path.open("rb", buffering=0)), info.sha256
        except FileNotFoundError:
            pass
        if not self._materialize_image(db, row["rowid"], info, path):
            return None
        return files.enter_context(path.open("rb", buffering=0)), info.sha256

    @staticmethod
    def _materialize_image(db, rowid, info, path):
        with db.blobopen("firmware_releases", "image", rowid, readonly=True) as blob:
            if len(blob) != info.size:
                return False
            with NamedTemporaryFile(dir=path.parent, delete=False) as stream:
                temporary = Path(stream.name)
                try:
                    digest = hashlib.sha256()
                    while chunk := blob.read(65536):
                        stream.write(chunk)
                        digest.update(chunk)
                    stream.flush()
                    os.fsync(stream.fileno())
                    if not info.matches_image(stream.tell(), digest.digest()):
                        return False
                    temporary.replace(path)
                    return True
                finally:
                    temporary.unlink(missing_ok=True)

    def boards(self, device):
        with closing(self.connect()) as db:
            states = db.execute(
                "SELECT * FROM firmware_devices WHERE device=? ORDER BY board", (device,)
            )
            return [
                (state, [r for r in self._releases(db, state["board"]) if r.supports(state["running"])])
                for state in states
            ]

    def approve(self, board, device, release_version):
        with closing(self.connect()) as db, db:
            db.execute("BEGIN IMMEDIATE")
            state = db.execute(
                "SELECT running FROM firmware_devices WHERE board=? AND device=?", (board, device)
            ).fetchone()
            if not state or not any(
                release.info.version == release_version and release.supports(state["running"])
                for release in self._releases(db, board)
            ):
                raise ReleaseError("This firmware update is no longer available for the board")
            db.execute(
                "UPDATE firmware_devices SET pending=?, pending_auto=0, approval=?, result=NULL WHERE board=? AND device=?",
                (release_version, secrets.token_hex(8), board, device),
            )

    def set_automatic(self, board, device, enabled):
        if board == "lcd43b" and enabled:
            raise ReleaseError("Confirm LCD firmware updates on the board")
        with closing(self.connect()) as db, db:
            updated = db.execute(
                """UPDATE firmware_devices SET automatic=?,
                   pending=CASE WHEN NOT ? AND pending_auto THEN NULL ELSE pending END,
                   pending_auto=CASE WHEN ? THEN pending_auto ELSE 0 END
                   WHERE board=? AND device=?""",
                (enabled, enabled, enabled, board, device),
            )
            if not updated.rowcount:
                raise ReleaseError("This board has not checked in")

    def checkin(self, board, device, running, result, result_approval=None):
        """Record telemetry and select this device's offer in one transaction."""
        if board not in BOARDS or device is None:
            return None
        with closing(self.connect()) as db, db:
            db.execute("BEGIN IMMEDIATE")
            db.execute("INSERT OR IGNORE INTO firmware_devices (board, device) VALUES (?,?)", (board, device))
            state = db.execute(
                "SELECT * FROM firmware_devices WHERE board=? AND device=?", (board, device)
            ).fetchone()
            pending, pending_auto, rolled_back = state["pending"], state["pending_auto"], state["rolled_back"]
            approval = state["approval"]
            completed = False
            if result:
                outcome, detail = result.split(":", 1)
                identified = approval is not None and result_approval == approval
                legacy = device != OPEN_DEVICE and result_approval is None
                if outcome == "rolled_back" and (identified or legacy) and (
                    rolled_back is None or version_tuple(detail) > version_tuple(rolled_back)
                ):
                    rolled_back = detail
                # An open-mode completion needs the current approval ID, including
                # when running telemetry would make the pending version ineligible.
                if outcome in ("ok", "rolled_back") and detail == pending and (identified or legacy):
                    pending, pending_auto = None, 0
                    completed = True
            running_now = running or state["running"]
            enabled = firmware_updates_enabled()
            eligible = {r.info.version: r for r in self._releases(db, board) if r.supports(running_now)}
            if enabled and device != OPEN_DEVICE and pending not in eligible:
                pending, pending_auto = None, 0
            if pending is None and board != "lcd43b" and state["automatic"] and running and not completed:
                pending = next((
                    v for v in eligible if rolled_back is None or version_tuple(v) > version_tuple(rolled_back)
                ), None)
                pending_auto = int(pending is not None)
                if pending_auto:
                    approval = secrets.token_hex(8)
            if pending is None:
                approval = None
            db.execute(
                """UPDATE firmware_devices SET running=?, result=?, pending=?, pending_auto=?, rolled_back=?, approval=?, last_seen=?
                   WHERE board=? AND device=?""",
                (running_now, result or state["result"], pending, pending_auto, rolled_back, approval,
                 datetime.now(timezone.utc).isoformat(), board, device),
            )
            return eligible[pending].manifest_entry(approval) if running and pending in eligible else None

    def last_poll(self):
        with closing(self.connect()) as db:
            return db.execute("SELECT checked, error FROM firmware_poll WHERE id=1").fetchone()

    def poll_due(self, now):
        previous = self.last_poll()
        return previous is None or now - datetime.fromisoformat(previous["checked"]) >= POLL_INTERVAL

    def record_poll(self, now, error):
        with closing(self.connect()) as db, db:
            db.execute("INSERT OR REPLACE INTO firmware_poll VALUES (1,?,?)", (now.isoformat(), error))


def _download(client, url, limit, *, asset=False):
    deadline = monotonic() + 60
    headers = {"User-Agent": f"Ventanal/{version('ventanal')}", "Accept-Encoding": "identity"}
    for _ in range(4):
        if monotonic() > deadline:
            raise ReleaseError("GitHub download timed out")
        with client.stream("GET", url, headers=headers) as response:
            if response.is_redirect and asset:
                try:
                    redirect = urljoin(url, response.headers.get("location", ""))
                    target = urlsplit(redirect)
                    if target.scheme != "https" or target.netloc not in ASSET_HOSTS:
                        raise ValueError
                    httpx.URL(redirect)
                except (ValueError, httpx.InvalidURL) as exc:
                    raise ReleaseError("Unexpected GitHub asset redirect") from exc
                url = redirect
                continue
            response.raise_for_status()
            data = bytearray()
            for chunk in response.iter_bytes(chunk_size=65536):
                if monotonic() > deadline:
                    raise ReleaseError("GitHub download timed out")
                if len(data) + len(chunk) > limit:
                    raise ReleaseError("GitHub response exceeds its size limit")
                data.extend(chunk)
            return bytes(data)
    raise ReleaseError("Too many GitHub asset redirects")


def _github_releases(client):
    try:
        listing = json.loads(_download(client, GITHUB_RELEASES + "?per_page=30", 2 * 1024 * 1024))
        if not isinstance(listing, list):
            raise ValueError
        candidates = []
        for item in listing:
            if not isinstance(item, dict) or not isinstance(item.get("tag_name"), str):
                raise ValueError
            tag = item["tag_name"]
            if not tag.startswith("v"):
                continue
            try:
                order = version_tuple(tag[1:])
            except ValueError:
                continue
            if item.get("draft") or item.get("prerelease"):
                continue
            assets = item.get("assets")
            if not isinstance(assets, list) or any(
                not isinstance(asset, dict) or not isinstance(asset.get("name"), str) for asset in assets
            ):
                raise ValueError
            candidates.append((order, tag[1:], {asset["name"] for asset in assets}))
        candidates.sort(key=lambda candidate: candidate[0], reverse=True)
        return [(tag, assets) for _, tag, assets in candidates]
    except (ValueError, RecursionError) as exc:
        if isinstance(exc, ReleaseError):
            raise
        raise ReleaseError("Malformed GitHub release list") from None


def _poll_error(exc):
    if isinstance(exc, httpx.HTTPStatusError):
        return f"GitHub returned HTTP {exc.response.status_code}"
    if isinstance(exc, httpx.TimeoutException):
        return "GitHub request timed out"
    if isinstance(exc, httpx.HTTPError):
        return "GitHub could not be reached"
    return str(exc)


def poll_github(store, client, devices, *, now=None):
    """Check recent public releases, retaining last-known-good images on errors."""
    now = now or datetime.now(timezone.utc)
    if not firmware_updates_enabled():
        return
    errors = []
    try:
        running = {board: [] for board in BOARDS}
        for device in devices:
            for state, _ in store.boards(device):
                running[state["board"]].append(state["running"])
        for release_version, assets in _github_releases(client):
            for board in BOARDS:
                stem = f"ventanal-{board}-{release_version}"
                names = {stem + suffix for suffix in (".json", ".json.sig", ".bin")}
                if not assets & names:
                    continue
                try:
                    if not names <= assets:
                        raise ReleaseError("Incomplete release assets")
                    base = f"{GITHUB_REPO}/releases/download/v{release_version}/{stem}"
                    metadata = _download(client, base + ".json", MAX_METADATA, asset=True)
                    signature = _download(client, base + ".json.sig", 88, asset=True)
                    release = verified_metadata(metadata, signature)
                    if (release.info.board, release.info.version) != (board, release_version):
                        raise ReleaseError("Signed metadata does not match the GitHub asset name")
                    if (any(release.supports(version) for version in running[board])
                            and not store.cached_metadata(release)):
                        image = _download(client, base + ".bin", release.info.size, asset=True)
                        store.cache(release, image)
                except httpx.HTTPStatusError as exc:
                    if exc.response.status_code != 404:
                        raise
                    errors.append(f"{stem}: GitHub asset not found")
                except ReleaseError as exc:
                    errors.append(f"{stem}: {exc}")
    except (httpx.HTTPError, ReleaseError) as exc:
        errors.append(_poll_error(exc))
    except (OSError, sqlite3.Error):
        errors.append("Firmware release check could not access the cache")
    error = "; ".join(errors[:5])
    if errors:
        log.warning("Firmware release check: %s", error)
    store.record_poll(now, error)
