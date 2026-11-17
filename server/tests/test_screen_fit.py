# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Stephen de la Vega
from dataclasses import replace
from datetime import datetime, timezone
from pathlib import Path
import random
from PIL import Image
from ventanal.art.pool import ArtPool
from ventanal.config import load_config
from test_art_colour import candidate


def test_score_sanity():
    from ventanal.art.screen_fit import assess

    palette = load_config(Path("ventanal.example.toml")).panel_rgb
    muted = assess(Image.new("RGB", (40, 30), palette[4]), palette)
    neon = assess(Image.new("RGB", (40, 30), (255, 0, 255)), palette)
    dark = assess(Image.new("RGB", (40, 30), (0, 0, 0)), palette)
    black = assess(Image.new("RGB", (40, 30), palette[0]), palette)
    assert muted.score > neon.score
    assert black.score > dark.score
    assert 0 <= neon.score <= 100


def test_palette_and_url_invalidation(tmp_path):
    pool = ArtPool(tmp_path / "art.db")
    c = candidate(kind="painting")
    pool.add([c], "winter")
    palette = load_config(Path("ventanal.example.toml")).panel_rgb
    pool.assess_fit(c, Image.new("RGB", (40, 30), "gray"), palette)
    assert pool.fit_score(c, palette) is not None
    assert pool.fit_score(c, tuple(reversed(palette))) is None
    pool.add([replace(c, image_url="https://example/new")], "winter")
    assert pool.fit_score(pool.get(c.source, c.object_id), palette) is None


def test_selection_and_best_fallback(tmp_path, caplog):
    from ventanal.art.screen_fit import fingerprint
    import sqlite3

    pool = ArtPool(tmp_path / "art.db")
    a, b = candidate("1", "painting"), candidate("2", "painting")
    pool.add([a, b], "winter")
    palette = load_config(Path("ventanal.example.toml")).panel_rgb
    with sqlite3.connect(pool.db_path) as db:
        db.execute(
            "UPDATE artworks SET fit_score=10, fit_palette=?", (fingerprint(palette),)
        )
        db.execute("UPDATE artworks SET fit_score=99 WHERE object_id='2'")

    def pick(**kw):
        return pool.pick_next(
            "winter",
            datetime.now(timezone.utc),
            0,
            random.Random(1),
            panel_rgb=palette,
            **kw
        )

    assert pick(screen_fit_only=True) == b
    assert pick(screen_fit_only=True, excluded=("met:2",)) == a
    assert "fallback" in caplog.text
    assert pick(screen_fit_only=False).object_id in {"1", "2"}
    assert pick(screen_fit_only=True, excluded=("met:1", "met:2")) is None


def test_setting_roundtrip(tmp_path):
    from ventanal.settings import save_settings
    from ventanal.ui import UI

    path = tmp_path / "settings.toml"
    path.write_text(Path("ventanal.example.toml").read_text())
    assert not load_config(path).screen_fit_only
    assert save_settings(path, {"art.screen_fit_only": "yes"}).screen_fit_only
    ui = object.__new__(UI)
    ui.config_path = path
    ui.render_result = ""
    assert "Only show art that suits this screen" in ui.settings_body("token")
    assert not save_settings(path, {"art.screen_fit_only": "no"}).screen_fit_only


def test_batch_has_hard_budget(tmp_path, monkeypatch):
    from ventanal.art import screen_fit
    import subprocess

    calls = []

    def run(args, **kwargs):
        calls.append(kwargs)
        raise subprocess.TimeoutExpired(args, kwargs["timeout"])

    monkeypatch.setattr(subprocess, "run", run)
    cfg = replace(load_config(Path("ventanal.example.toml")), cache_dir=tmp_path)
    screen_fit.batch(cfg)
    assert calls[0]["timeout"] == screen_fit.BUDGET_SECONDS
    assert calls[0]["stdout"] == subprocess.DEVNULL
    assert calls[0]["stderr"] is None


def test_failed_batch_is_retryable(tmp_path, monkeypatch):
    from ventanal.art import screen_fit, sources
    import sqlite3

    c = candidate(kind="painting")
    pool = ArtPool(tmp_path / "art.sqlite3")
    pool.add([c], "winter")
    palette = load_config(Path("ventanal.example.toml")).panel_rgb

    def unavailable(*args, **kw):
        raise ValueError("private upstream detail")

    monkeypatch.setattr(sources, "fetch_image", unavailable)
    screen_fit._worker(str(tmp_path), palette, ["met"])
    assert pool.fit_score(c, palette) is None
    with sqlite3.connect(pool.db_path) as db:
        db.execute("UPDATE artworks SET fit_retry_after=0")
    monkeypatch.setattr(
        sources, "fetch_image", lambda *a, **kw: Image.new("RGB", (40, 30), "gray")
    )
    screen_fit._worker(str(tmp_path), palette, ["met"])
    assert pool.fit_score(c, palette) is not None


from test_jobs import setup_job, NOW


def test_enabling_filter_reselects_forced_main(setup_job, monkeypatch):
    from ventanal import jobs
    from ventanal.art import screen_fit
    from ventanal.manifest import read_manifest
    import httpx, sqlite3

    cfg = setup_job
    pool = ArtPool(cfg.cache_dir / "art.sqlite3")
    good = candidate("2", "painting")
    pool.add([good], "holiday-winter")
    monkeypatch.setattr(screen_fit, "batch", lambda cfg: None)
    monkeypatch.setattr(
        jobs,
        "render_art",
        lambda image, c: (
            object(),
            {"source": c.source, "object_id": c.object_id, "title": c.title},
        ),
    )
    with sqlite3.connect(pool.db_path) as db:
        db.execute(
            "UPDATE artworks SET fit_score=10,fit_palette=?",
            (screen_fit.fingerprint(cfg.panel_rgb),),
        )
        db.execute("UPDATE artworks SET fit_score=99 WHERE object_id='2'")
    with httpx.Client() as client:
        jobs.tick(replace(cfg, ignored=("met:2",)), NOW, client)
        assert (
            read_manifest(cfg.output_dir)["screens"]["art"]["credit"]["object_id"]
            == "1"
        )
        jobs.tick(replace(cfg, screen_fit_only=True), NOW, client, force_art=True)
    assert read_manifest(cfg.output_dir)["screens"]["art"]["credit"]["object_id"] == "2"


from test_gui import web


def test_art_page_reads_cached_scores_only(web, monkeypatch):
    from test_gui2 import ready
    from ventanal.art import screen_fit
    import sqlite3

    request, server, root, token = ready(web, "/art")
    cfg = load_config(root / "settings.toml")
    pool = ArtPool(root / "cache/art.sqlite3")
    c = candidate("123", "painting")
    pool.add([c], "winter")
    credit = {"source": c.source, "object_id": c.object_id, "title": c.title}
    server.ui.store.put_spare(
        1, {"id": "a" * 16, "url": "/frames/spare.vnf", "credit": credit}, c
    )
    server.ui.store.register("b" * 16, c)
    server.ui.store.record("a" * 16, "b" * 16, datetime.now(timezone.utc))
    with sqlite3.connect(pool.db_path) as db:
        db.execute(
            "UPDATE artworks SET fit_score=93,fit_palette=?",
            (screen_fit.fingerprint(cfg.panel_rgb),),
        )

    def forbidden(*a, **kw):
        raise AssertionError("HTTP must never score images")

    monkeypatch.setattr(screen_fit, "assess", forbidden)
    status, body, _ = request("/art")
    assert status == 200
    assert body.count("Screen fit 93") == 2
    assert (
        request(
            "/settings",
            {"csrf": token, "monotone.present": "yes", "art.screen_fit_only": "yes"},
        )[0]
        == 200
    )
    assert load_config(root / "settings.toml").screen_fit_only
    assert request("/settings", {"csrf": token, "monotone.present": "yes"})[0] == 200
    assert load_config(root / "settings.toml").screen_fit_only
    assert request("/settings", {"csrf": token, "screen_fit.present": "yes"})[0] == 200
    assert not load_config(root / "settings.toml").screen_fit_only
    assert (
        request(
            "/settings",
            {
                "csrf": token,
                "art.screen_fit_only.present": "yes",
                "art.screen_fit_only": "yes",
            },
        )[0]
        == 200
    )
    assert load_config(root / "settings.toml").screen_fit_only
    assert (
        request("/settings", {"csrf": token, "art.include_monotone.present": "yes"})[0]
        == 200
    )
    assert load_config(root / "settings.toml").screen_fit_only
    assert (
        request("/settings", {"csrf": token, "art.screen_fit_only.present": "yes"})[0]
        == 200
    )
    assert not load_config(root / "settings.toml").screen_fit_only


def test_forced_spare_keeps_better_fallback(setup_job, monkeypatch):
    from ventanal import jobs
    from ventanal.art import screen_fit
    from ventanal.owner import OwnerStore
    import httpx, sqlite3

    cfg = replace(setup_job, screen_fit_only=True)
    pool = ArtPool(cfg.cache_dir / "art.sqlite3")
    a, b = candidate("2", "painting"), candidate("3", "painting")
    pool.add([a, b], "holiday-winter")
    monkeypatch.setattr(screen_fit, "batch", lambda cfg: None)
    monkeypatch.setattr(
        jobs,
        "render_art",
        lambda image, c: (
            object(),
            {"source": c.source, "object_id": c.object_id, "title": c.title},
        ),
    )
    with sqlite3.connect(pool.db_path) as db:
        db.execute(
            "UPDATE artworks SET fit_score=99,fit_palette=?",
            (screen_fit.fingerprint(cfg.panel_rgb),),
        )
        db.execute("UPDATE artworks SET fit_score=10 WHERE object_id='2'")
        db.execute("UPDATE artworks SET fit_score=5 WHERE object_id='3'")
    owner = OwnerStore(cfg.cache_dir)
    owner.put_spare(
        0,
        {
            "id": "a" * 16,
            "url": "/frames/spare.vnf",
            "credit": {"source": a.source, "object_id": a.object_id},
        },
        a,
    )
    with httpx.Client() as client:
        # Publish main while the lower-scoring works are reserved/excluded.
        jobs.tick(replace(cfg, ignored=("met:3",)), NOW, client, force_art=False)
        jobs.tick(cfg, NOW, client, force_spares=True)
    assert OwnerStore(cfg.cache_dir).spares()[0][3] == "2"


def test_failure_backoff_persists_caps_and_resets(tmp_path, monkeypatch, caplog):
    import sqlite3
    import time
    from ventanal.art import screen_fit, sources

    pool = ArtPool(tmp_path / "art.sqlite3")
    c = candidate(kind="painting")
    pool.add([c], "winter")
    palette = load_config(Path("ventanal.example.toml")).panel_rgb
    clock = [1000.0]
    calls = []
    monkeypatch.setattr(time, "time", lambda: clock[0])

    def unavailable(*args, **kwargs):
        calls.append(1)
        raise ValueError("https://private.example/secret?token=credential")

    monkeypatch.setattr(sources, "fetch_image", unavailable)
    for failure in range(1, 13):
        screen_fit._worker(str(tmp_path), palette, ["met"])
        with sqlite3.connect(pool.db_path) as db:
            count, retry = db.execute(
                "SELECT fit_failures, fit_retry_after FROM artworks"
            ).fetchone()
        assert count == failure
        assert retry - clock[0] == min(300 * 2 ** (failure - 1), 86400)
        # Reopen the DB/worker: cooldown must survive process restarts.
        screen_fit._worker(str(tmp_path), palette, ["met"])
        assert len(calls) == failure
        clock[0] = retry
    assert "Screen-fit image unavailable for source met" in caplog.text
    assert "private.example" not in caplog.text and "credential" not in caplog.text
    monkeypatch.setattr(
        sources, "fetch_image", lambda *a, **kw: Image.new("RGB", (40, 30), "gray")
    )
    screen_fit._worker(str(tmp_path), palette, ["met"])
    with sqlite3.connect(pool.db_path) as db:
        assert db.execute(
            "SELECT fit_failures, fit_retry_after FROM artworks"
        ).fetchone() == (0, None)
    assert pool.fit_score(c, palette) is not None
    # Changed URLs recover immediately even after a failed attempt.
    with sqlite3.connect(pool.db_path) as db:
        db.execute("UPDATE artworks SET fit_failures=9, fit_retry_after=999999")
    pool.add([replace(c, image_url="https://example/new")], "winter")
    with sqlite3.connect(pool.db_path) as db:
        assert db.execute(
            "SELECT fit_failures, fit_retry_after FROM artworks"
        ).fetchone() == (0, None)


def test_interrupted_scoring_has_persistent_cooldown(tmp_path, monkeypatch):
    import sqlite3
    import pytest
    from ventanal.art import screen_fit, sources

    pool = ArtPool(tmp_path / "art.sqlite3")
    pool.add([candidate(kind="painting")], "winter")

    def killed(*a, **kw):
        raise KeyboardInterrupt()

    monkeypatch.setattr(sources, "fetch_image", killed)
    palette = load_config(Path("ventanal.example.toml")).panel_rgb
    with pytest.raises(KeyboardInterrupt):
        screen_fit._worker(str(tmp_path), palette, ["met"])
    with sqlite3.connect(pool.db_path) as db:
        count, retry, attempted = db.execute(
            "SELECT fit_failures, fit_retry_after, fit_attempt FROM artworks"
        ).fetchone()
    assert count == 1 and retry - attempted == 300
    # The second worker must skip the interrupted artwork.
    screen_fit._worker(str(tmp_path), palette, ["met"])


def test_strict_empty_season_uses_best_entries_and_logs(tmp_path, caplog):
    import sqlite3
    from ventanal.art.screen_fit import fingerprint

    pool = ArtPool(tmp_path / "art.sqlite3")
    a, b = candidate("1", "painting"), candidate("2", "painting")
    pool.add([a, b], "summer")
    palette = load_config(Path("ventanal.example.toml")).panel_rgb

    def pick(**kw):
        return pool.pick_next(
            "winter",
            NOW,
            30,
            random.Random(1),
            strict=True,
            screen_fit_only=True,
            panel_rgb=palette,
            **kw
        )

    assert pick() in (a, b)
    assert "unscored provisional" in caplog.text
    assert "fallback" not in caplog.text
    caplog.clear()
    with sqlite3.connect(pool.db_path) as db:
        db.execute(
            "UPDATE artworks SET fit_score=10, fit_palette=?", (fingerprint(palette),)
        )
        db.execute("UPDATE artworks SET fit_score=80 WHERE object_id='2'")
    assert pick() == b
    assert "strict selection empty" in caplog.text and "fallback" in caplog.text
    assert pick(excluded=("met:2",)) == a
    assert pick(excluded=("met:1", "met:2")) is None


def test_scoring_spawn_failure_does_not_abort_tick(setup_job, monkeypatch, caplog):
    import httpx
    from ventanal.art import screen_fit
    from ventanal import jobs

    def fail(cfg):
        raise OSError("private spawn details")

    monkeypatch.setattr(screen_fit, "batch", fail)
    with httpx.Client() as client:
        result = jobs.tick(setup_job, NOW, client)
    assert result["art"] and result["weather"]
    assert "Screen-fit batch unavailable" in caplog.text
    assert "private spawn details" not in caplog.text


def test_boolean_markers_derive_from_field_key(tmp_path):
    from ventanal.ui import UI

    path = tmp_path / "settings.toml"
    path.write_text(Path("ventanal.example.toml").read_text())
    ui = object.__new__(UI)
    ui.config_path, ui.render_result = path, ""
    body = ui.settings_body("token")
    assert 'name="art.screen_fit_only.present"' in body
    assert 'name="art.include_monotone.present"' in body
