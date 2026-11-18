# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Stephen de la Vega

"""Publish encoded frames, previews and the firmware's version 1 manifest."""

from dataclasses import asdict, dataclass
from datetime import datetime
import logging
import json
import os
import re
from pathlib import Path
from typing import Literal

from PIL import Image

from ventanal.frame import frame_id


MAX_SERVED_MANIFEST_BYTES = 6144
CREDIT_FIELD_LIMITS = {"artist": 120, "date": 120, "museum": 120}
_CREDIT_CONTROLS = re.compile(r"[\x00-\x1f\x7f-\x9f]")


def clean_credit_text(value: str, limit: int | None = None) -> str:
    return _CREDIT_CONTROLS.sub("", value)[:limit]


def _clean_credit(credit: dict | None) -> dict | None:
    if not isinstance(credit, dict):
        return credit
    return {
        key: clean_credit_text(value, CREDIT_FIELD_LIMITS.get(key))
        if key in ("title", "secondary", *CREDIT_FIELD_LIMITS) and isinstance(value, str)
        else value
        for key, value in credit.items()
    }


def _entries(manifest: dict):
    yield from manifest.get("screens", {}).values()
    yield from manifest.get("spares", [])


def omit_board_credits(manifest: dict) -> None:
    """The artwork already has its credits rendered into the frame."""
    for entry in _entries(manifest):
        entry.pop("credit", None)


@dataclass(frozen=True)
class Readout:
    x: int
    y: int
    w: int
    h: int
    style: Literal["lcars", "shade"]


def read_manifest(out_dir: Path, *, store=None) -> dict:
    """Recover an unusable manifest so a power loss cannot wedge publication."""
    try:
        manifest = json.loads((out_dir / "manifest.json").read_text(encoding="utf-8"))
        if (not isinstance(manifest, dict) or manifest.get("version") != 1
                or not isinstance(manifest.get("screens"), dict)
                or any(not isinstance(entry, dict) for entry in manifest["screens"].values())):
            raise ValueError("invalid manifest structure")
        return manifest
    except FileNotFoundError:
        pass
    except (OSError, ValueError):
        detail = "manifest.json unreadable; treating as empty"
        logging.getLogger(__name__).warning(detail)
        if store is not None:
            try:
                store.event("manifest recovery", detail)
            except OSError:
                logging.getLogger(__name__).warning("Manifest recovery event could not be stored")
    return {"version": 1, "screens": {}}


def publish(
    out_dir: Path,
    screen: str,
    data: bytes,
    readout: Readout,
    credit: dict | None,
    preview: Image.Image,
    now: datetime,
) -> dict:
    """Publish supplied assets before atomically replacing the manifest.

    Callers serialize publishes. Retention follows file modification time,
    with the frame being published always retained as the newest.
    """
    if now.tzinfo is None or now.utcoffset() is None:
        raise ValueError("now must be timezone-aware")
    if screen not in (
        "art",
        "weather",
        "solar",
        "spare0",
        "spare1",
        "spare2",
    ) and not re.fullmatch(r"extra-[a-z0-9_-]{1,42}", screen):
        raise ValueError("screen must be art, weather, solar, spare0–2, or a valid extra-* name")

    manifest = read_manifest(out_dir)
    ident = frame_id(data)
    basename = f"{screen}-{ident}"
    entry = {
        "id": ident,
        "url": f"/frames/{basename}.vnf",
        "size": len(data),
        "rendered": now.isoformat(timespec="seconds"),
        "readout": asdict(readout),
        "credit": _clean_credit(credit),
    }
    frames = out_dir / "frames"
    previews = out_dir / "previews"
    frames.mkdir(parents=True, exist_ok=True)
    previews.mkdir(parents=True, exist_ok=True)
    frame_path = frames / f"{basename}.vnf"
    frame_path.write_bytes(data)
    preview.save(previews / f"{basename}.png", format="PNG")

    if screen.startswith("spare"):
        # The durable ledger assembles the public list once slots are ready.
        pass
    else:
        manifest["screens"][screen] = entry
    write_manifest(out_dir, manifest)

    # Prune only after the new manifest is visible, preserving its assets.
    newest = sorted(
        frames.glob(f"{screen}-*.vnf"),
        key=lambda path: (path == frame_path, path.stat().st_mtime_ns, path.name),
        reverse=True,
    )
    for old in newest[4:]:
        old.unlink()
        (previews / f"{old.stem}.png").unlink(missing_ok=True)
    return entry


def write_manifest(out_dir: Path, manifest: dict) -> None:
    """Atomically update optional metadata under the caller's publication lock."""
    out_dir.mkdir(parents=True, exist_ok=True)
    order_screens(manifest)
    for entry in _entries(manifest):
        if "credit" in entry:
            entry["credit"] = _clean_credit(entry["credit"])
    temporary = out_dir / "manifest.json.tmp"
    try:
        with temporary.open("w", encoding="utf-8") as stream:
            stream.write(json.dumps(manifest, indent=2) + "\n")
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, out_dir / "manifest.json")
        directory = os.open(out_dir, os.O_RDONLY)
        try:
            os.fsync(directory)
        finally:
            os.close(directory)
    finally:
        temporary.unlink(missing_ok=True)


def order_screens(manifest):
    if "screen_order" in manifest:
        screens = manifest.get("screens", {})
        manifest["screen_order"] = [
            s
            for s in (
                "art",
                "weather",
                "solar",
                *[s for s in screens if s.startswith("extra-")],
            )
            if s in screens
        ]
