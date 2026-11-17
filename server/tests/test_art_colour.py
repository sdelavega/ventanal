# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Stephen de la Vega
"""Colour policy, migration and safe source diagnostics."""

from dataclasses import replace
from datetime import datetime, timezone
from pathlib import Path
import random
import sqlite3

import httpx
from PIL import Image
import pytest

from ventanal.art.sources.base import Candidate
from ventanal.art.pool import ArtPool
from ventanal.art.sources import registry, search_all
from ventanal.config import load_config


def candidate(ident='1', kind='other'):
    return Candidate(
        'met',
        ident,
        'Title',
        '',
        '',
        'Museum',
        'https://example/' + ident,
        2000,
        1800,
        kind=kind,
    )


@pytest.mark.parametrize(
    'metadata,expected',
    [
        ('Oil on canvas', 'painting'),
        ('etching', 'print'),
        ('gelatin silver photograph', 'photograph'),
        ('watercolour drawing', 'drawing'),
        ('illuminated manuscript', 'manuscript/illumination'),
        ('miniature painting', 'manuscript/illumination'),
        ('sculpture', 'other'),
    ],
)
def test_kind(metadata, expected):
    from ventanal.art.sources.base import classify_kind

    assert classify_kind(metadata) == expected


def test_chroma_rejects_neutral_and_sepia_but_accepts_colour():
    from ventanal.art.colour import is_colourful

    assert not is_colourful(Image.new('RGB', (80, 60), 'gray'))
    assert not is_colourful(Image.new('RGB', (80, 60), (180, 170, 150)))
    # Yellowed etching paper from the rejected NGA sample must not qualify.
    assert not is_colourful(Image.new('RGB', (80, 60), (205, 190, 145)))
    assert not is_colourful(Image.new('RGB', (80, 60), 'red'))
    assert not is_colourful(Image.new('RGB', (80, 60), 'blue'))
    image = Image.new('RGB', (80, 60), 'red')
    image.paste('blue', (40, 0, 80, 60))
    assert is_colourful(image)


def test_selection_retries_monochrome_and_preserves_pool(tmp_path, monkeypatch):
    from ventanal import jobs

    cfg = replace(
        load_config(Path('ventanal.example.toml')), cache_dir=tmp_path, museums=('met',)
    )
    pool = ArtPool(tmp_path / 'art.sqlite3')
    pool.add([candidate('1', 'print'), candidate('2', 'painting')], 'winter')
    calls = []

    def fetch(c, client, cfg):
        calls.append(c.object_id)
        return Image.new('RGB', (80, 60), 'gray' if c.object_id == '1' else 'red')

    monkeypatch.setattr(jobs, 'fetch_candidate', fetch)
    now = datetime.now(timezone.utc)
    pool.assess(candidate('1', 'print'), Image.new('RGB', (80, 60), 'gray'))
    c, image = jobs.select_artwork(cfg, now, None, pool, excluded=(), strict=False)
    assert c.object_id == '2'
    assert pool.get('met', '1') is not None
    assert pool.get('met', '2').kind == 'painting'
    assert (
        pool.pick_next(
            'winter',
            now,
            0,
            random.Random(),
            excluded=('met:2',),
            include_monotone=False,
        )
        is None
    )
    assert (
        pool.pick_next(
            'winter',
            now,
            0,
            random.Random(),
            excluded=('met:2',),
            include_monotone=True,
        ).object_id
        == '1'
    )


def test_aic_rights_fail_closed():
    assert not registry()['aic'].rights_filter({})
    assert registry()['aic'].rights_filter({'is_public_domain': True})


def test_fetch_requires_cache_dir():
    with pytest.raises(ValueError, match='cache_dir'):
        registry()['smk'].fetch_image(candidate(), None)


def test_local_traceback_without_upstream_secret(caplog):
    secret = 'upstream-body-and-token'

    class Broken:
        id = name = 'broken'

        def search(self, *args):
            raise ValueError(secret)

    assert search_all([Broken()], ['winter'], 1, None) == []
    assert caplog.records[0].exc_info
    assert 'upstream-body-and-token' not in caplog.text
    assert 'search' in caplog.text


def test_setting_default_and_checkbox(tmp_path):
    from ventanal.settings import save_settings
    from ventanal.ui import UI

    path = tmp_path / 'settings.toml'
    path.write_text(Path('ventanal.example.toml').read_text())
    assert load_config(path).include_monotone is False
    assert save_settings(path, {'art.include_monotone': 'yes'}).include_monotone is True
    ui = object.__new__(UI)
    ui.config_path = path
    ui.render_result = ''
    body = ui.settings_body('token')
    assert (
        'Include monotone works (black-and-white or single-colour prints, drawings and photographs)'
        in body
    )
    assert 'name="art.include_monotone" value="yes" checked' in body
    assert save_settings(path, {'art.include_monotone': 'no'}).include_monotone is False


def test_migration_preserves_history_and_invalidates_changed_image(tmp_path):
    path = tmp_path / 'old.sqlite3'
    with sqlite3.connect(path) as db:
        db.execute(
            'CREATE TABLE artworks (source TEXT, object_id TEXT, title TEXT, artist TEXT, date TEXT, museum TEXT, image_url TEXT, width INTEGER, height INTEGER, last_shown TEXT, PRIMARY KEY(source, object_id))'
        )
        db.execute(
            "INSERT INTO artworks VALUES ('met','1','Title','','','Museum','https://example/1',2000,1800,'2026-01-01T00:00:00+00:00')"
        )
    pool = ArtPool(path)
    c = pool.get('met', '1')
    assert c.kind == 'other'
    assert not pool.assess(c, Image.new('RGB', (20, 20), 'gray'))
    pool.add([replace(c, kind='print')], 'winter')
    with sqlite3.connect(path) as db:
        row = db.execute(
            'SELECT last_shown, colour_fraction, kind FROM artworks'
        ).fetchone()
    assert row == ('2026-01-01T00:00:00+00:00', None, 'print')
    pool.add([replace(c, image_url='https://example/new')], 'winter')
    with sqlite3.connect(path) as db:
        assert db.execute('SELECT colour_fraction FROM artworks').fetchone()[0] is None


@pytest.mark.parametrize(
    'source',
    ('met', 'aic', 'cleveland', 'rijksmuseum', 'nga', 'smk', 'commons', 'paris'),
)
def test_every_source_obeys_selection_policy(tmp_path, monkeypatch, source):
    from ventanal import jobs

    cfg = replace(
        load_config(Path('ventanal.example.toml')),
        cache_dir=tmp_path,
        museums=(source,),
    )
    pool = ArtPool(tmp_path / 'art.sqlite3')
    c = replace(candidate(), source=source)
    pool.add([c], 'holiday-winter')
    monkeypatch.setattr(
        jobs, 'fetch_candidate', lambda *args: Image.new('RGB', (20, 20), 'gray')
    )
    with pytest.raises(ValueError, match='eligible'):
        jobs.select_artwork(cfg, datetime.now(timezone.utc), None, pool)
    assert (
        jobs.select_artwork(
            replace(cfg, include_monotone=True),
            datetime.now(timezone.utc),
            None,
            pool,
        )[0]
        == c
    )


def test_assessments_invalidated_when_policy_changes(tmp_path):
    path = tmp_path / 'art.sqlite3'
    pool = ArtPool(path)
    c = candidate()
    pool.add([c], 'winter')
    pool.assess(c, Image.new('RGB', (20, 20), 'gray'))
    with sqlite3.connect(path) as db:
        db.execute("UPDATE pool_metadata SET value='old' WHERE key='colour_policy'")
    assert ArtPool(path).colour_status(c) is None


@pytest.mark.parametrize('kind', ('painting', 'manuscript/illumination'))
def test_metadata_preserves_muted_art(tmp_path, kind):
    pool = ArtPool(tmp_path / 'art.sqlite3')
    c = candidate(kind=kind)
    pool.add([c], 'winter')
    assert pool.assess(c, Image.new('RGB', (40, 40), (90, 70, 50)))
    assert pool.colour_status(c) is True


@pytest.mark.parametrize('colour', ((145, 85, 30), (20, 100, 170), (180, 100, 90)))
def test_single_hue_tints_are_monotone(colour):
    from PIL import ImageOps
    from ventanal.art.colour import is_colourful

    gradient = Image.linear_gradient('L').resize((100, 100))
    assert not is_colourful(ImageOps.colorize(gradient, colour, 'white'))


# Exercise publication and existing spare replacement with the production policy.
from test_jobs import setup_job, NOW


def test_tick_replaces_monotone_spares_and_preserves_rows(setup_job, monkeypatch):
    from ventanal import jobs
    from ventanal.manifest import read_manifest
    from ventanal.owner import OwnerStore

    cfg = replace(setup_job, include_monotone=True, weather_interval_minutes=10000)
    pool = ArtPool(cfg.cache_dir / 'art.sqlite3')
    works = [candidate(str(i), 'print') for i in range(2, 6)]
    pool.add(works, 'holiday-winter')
    monkeypatch.setattr(
        jobs, 'fetch_candidate', lambda *a: Image.new('RGB', (16, 12), (140, 80, 30))
    )
    monkeypatch.setattr(
        jobs,
        'render_art',
        lambda image, c: (
            object(),
            {'source': c.source, 'object_id': c.object_id, 'title': c.title},
        ),
    )
    with httpx.Client() as client:
        jobs.tick(cfg, NOW, client)
        before = OwnerStore(cfg.cache_dir).spares()
        assert len(before) == 3
        assert any(pool.get(row[2], row[3]).kind == 'print' for row in before)
        pool.add(
            [candidate(str(i), 'painting') for i in range(6, 12)], 'holiday-winter'
        )
        jobs.tick(replace(cfg, include_monotone=False), NOW, client)
    after = OwnerStore(cfg.cache_dir).spares()
    assert len(after) == 3
    assert all(pool.get(row[2], row[3]).kind == 'painting' for row in after)
    assert all(pool.get(c.source, c.object_id) is not None for c in works)
    assert len(read_manifest(cfg.output_dir)['spares']) == 3


def test_kind_change_invalidates_metadata_exemption(tmp_path):
    pool = ArtPool(tmp_path / 'art.sqlite3')
    c = candidate(kind='painting')
    pool.add([c], 'winter')
    assert pool.assess(c, Image.new('RGB', (20, 20), 'gray'))
    reclassified = replace(c, kind='print')
    pool.add([reclassified], 'winter')
    assert pool.colour_status(reclassified) is None
    assert not pool.assess(reclassified, Image.new('RGB', (20, 20), 'gray'))
