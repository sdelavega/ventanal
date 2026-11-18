# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Stephen de la Vega

from dataclasses import FrozenInstanceError
from datetime import datetime, timedelta, timezone
import json

import numpy as np
from PIL import Image
import pytest

from ventanal.frame import encode_frame, frame_id
from ventanal.art.sources import Candidate
from ventanal.manifest import Readout, publish, read_manifest, write_manifest


NOW = datetime(2026, 11, 15, 14, 30, tzinfo=timezone(timedelta(hours=-5)))
ART = Readout(1020, 0, 580, 104, "shade")
WEATHER = Readout(1236, 30, 334, 136, "lcars")
CREDIT = {"title": "Title", "artist": "Artist", "date": "1900", "museum": "Museum"}


def frame(value=0):
    return encode_frame(np.full((1200, 1600), value, dtype=np.uint8))


def publish_art(out_dir, data=None, now=NOW):
    return publish(out_dir, "art", frame() if data is None else data, ART,
                   CREDIT, Image.new("RGB", (16, 12), "red"), now)


def test_read_missing_manifest(tmp_path):
    assert read_manifest(tmp_path) == {"version": 1, "screens": {}}


def test_publish_keeps_other_screen(tmp_path):
    weather = publish(tmp_path, "weather", frame(1), WEATHER, None,
                      Image.new("RGB", (16, 12)), NOW)
    art = publish_art(tmp_path)
    manifest = read_manifest(tmp_path)
    assert manifest == {"version": 1, "screens": {"weather": weather, "art": art}}
    assert manifest["screens"]["weather"]["id"] == weather["id"]
    assert weather["credit"] is None
    assert weather["readout"] == {"x": 1236, "y": 30, "w": 334, "h": 136, "style": "lcars"}


def test_publish_exact_entry_and_assets(tmp_path):
    data = frame()
    ident = frame_id(data)
    entry = publish_art(tmp_path, data, NOW.replace(microsecond=123456))
    assert entry == {
        "id": ident, "url": f"/frames/art-{ident}.vnf", "size": 960016,
        "rendered": "2026-11-15T14:30:00-05:00",
        "readout": {"x": 1020, "y": 0, "w": 580, "h": 104, "style": "shade"},
        "credit": CREDIT,
    }
    assert (tmp_path / entry["url"].lstrip("/")).read_bytes() == data
    with Image.open(tmp_path / "previews" / f"art-{ident}.png") as preview:
        assert preview.format == "PNG"
        assert preview.size == (16, 12)
        assert preview.getpixel((0, 0)) == (255, 0, 0)


def test_credit_cleaned_at_ingestion_and_publication(tmp_path):
    controls = ''.join(chr(c) for c in (*range(32), *range(127, 160)))
    text = controls + 'é' * 130
    candidate = Candidate('met', '123', 'Title' + controls, text, text, text, '', None, None)
    assert candidate.title == 'Title'
    assert candidate.artist == candidate.date == candidate.museum == 'é' * 120

    raw = dict.fromkeys(('title', 'secondary', 'artist', 'date', 'museum'), text)
    raw.update(source='met', object_id='123', x=28)
    cleaned = raw | dict.fromkeys(('title', 'secondary'), 'é' * 130)
    cleaned.update(dict.fromkeys(('artist', 'date', 'museum'), 'é' * 120))
    art = publish(tmp_path, 'art', frame(), ART, raw, Image.new('RGB', (16, 12)), NOW)
    assert art['credit'] == cleaned
    assert raw['artist'] == text

    manifest = read_manifest(tmp_path)
    manifest['spares'] = [art | {'credit': raw}]  # Simulate an older spare ledger entry.
    write_manifest(tmp_path, manifest)
    assert read_manifest(tmp_path)['spares'][0]['credit'] == cleaned


def test_publish_prunes(tmp_path):
    weather = publish(tmp_path, "weather", frame(), WEATHER, None,
                      Image.new("RGB", (16, 12)), NOW)
    entries = [publish_art(tmp_path, frame(i)) for i in range(6)]
    expected = {f"art-{entry['id']}.vnf" for entry in entries[-4:]}
    assert {p.name for p in (tmp_path / "frames").glob("art-*")} == expected
    assert {p.stem for p in (tmp_path / "previews").glob("art-*.png")} == {
        p.removesuffix(".vnf") for p in expected
    }
    assert (tmp_path / weather["url"].lstrip("/")).exists()
    assert read_manifest(tmp_path)["screens"]["weather"] == weather
    assert read_manifest(tmp_path)["screens"]["art"] == entries[-1]


def test_publish_atomic(tmp_path, monkeypatch):
    from ventanal import manifest as module

    previous = publish_art(tmp_path)
    replace = module.os.replace
    calls = []

    def observe(source, destination):
        assert source == tmp_path / "manifest.json.tmp"
        assert destination == tmp_path / "manifest.json"
        assert read_manifest(tmp_path)["screens"]["art"] == previous
        assert json.loads(source.read_text())["screens"]["art"] != previous
        calls.append((source, destination))
        replace(source, destination)

    monkeypatch.setattr(module.os, "replace", observe)
    entry = publish_art(tmp_path, frame(1))
    assert len(calls) == 1
    assert not (tmp_path / "manifest.json.tmp").exists()
    assert json.loads((tmp_path / "manifest.json").read_text())["screens"]["art"] == entry


def test_publish_requires_aware_time(tmp_path):
    with pytest.raises(ValueError, match="timezone-aware"):
        publish_art(tmp_path, now=NOW.replace(tzinfo=None))
    assert not (tmp_path / "manifest.json").exists()


def test_readout_is_frozen():
    with pytest.raises(FrozenInstanceError):
        ART.x = 0
