# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Stephen de la Vega

from dataclasses import replace
from datetime import date, datetime, timedelta, timezone
from pathlib import Path
import random

import httpx
from PIL import Image
import pytest

from ventanal.art.pool import ArtPool, refresh_pool, season_for
from ventanal.art.sources import Candidate, SOURCES
from ventanal.config import Season, load_config


NOW = datetime(2026, 11, 15, 12, tzinfo=timezone.utc)
EXAMPLE = Path(__file__).parents[1] / 'ventanal.example.toml'


def candidate(ident, width=2000, height=1500):
    return Candidate('met', ident, f'Artwork {ident}', 'Artist', '1900',
                     'Museum', f'https://example.test/{ident}.jpg', width, height)


@pytest.fixture
def pool(tmp_path):
    return ArtPool(tmp_path / 'art.sqlite3')


def pick(pool, season='winter', now=NOW, rng=None):
    return pool.pick_next(season, now, 60, rng or random.Random(42))


def test_season_wraps_year():
    winter = Season('winter', (12, 1), (2, 28), ('snow',))
    seasons = [winter, Season('rest', (3, 1), (11, 30), ('flowers',))]
    assert season_for(date(2027, 1, 15), seasons) == winter
    assert season_for(date(2026, 12, 1), seasons) == winter
    assert season_for(date(2027, 2, 28), seasons) == winter


def test_spec_defaults():
    cfg = load_config(EXAMPLE)
    assert season_for(date(2026, 11, 15), cfg.seasons).name == 'holiday-winter'
    assert season_for(date(2026, 4, 1), cfg.seasons).name == 'spring'


def test_season_first_match_and_no_match():
    first = Season('first', (1, 1), (1, 31), ('ice',))
    second = replace(first, name='second')
    assert season_for(date(2026, 1, 1), [first, second]) == first
    assert season_for(date(2026, 1, 31), [first]) == first
    with pytest.raises(ValueError):
        season_for(date(2026, 2, 1), [first])
    with pytest.raises(ValueError):
        season_for(date(2026, 1, 1), [])


def test_no_repeat(pool):
    first, second = candidate('1'), candidate('2')
    pool.add([first, second], 'winter')
    pool.mark_shown(first, NOW)
    assert pick(pool) == second
    assert pick(pool, now=NOW + timedelta(days=60, microseconds=-1)) == second
    pool.mark_shown(second, NOW + timedelta(days=1))
    assert pick(pool, now=NOW + timedelta(days=60)) == first


def test_fallbacks(pool):
    first, second = candidate('1'), candidate('2')
    assert pick(pool) is None
    pool.add([first], 'autumn')
    assert pick(pool) == first
    pool.add([second], 'summer')
    pool.mark_shown(first, NOW - timedelta(days=2))
    pool.mark_shown(second, NOW - timedelta(days=1))
    assert pick(pool) == first


def test_small_images_skipped(pool):
    small, large = candidate('small', 1200, 900), candidate('large')
    pool.add([small, large], 'winter')
    for seed in range(30):
        assert pick(pool, rng=random.Random(seed)) == large


@pytest.mark.parametrize(('width', 'height', 'allowed'), [
    (3010, 1000, False),
    (1000, 3010, False),
    (3000, 1000, True),
    (1000, 3000, True),
])
def test_art_aspect_limit_applies_to_all_pool_fallbacks(pool, width, height, allowed):
    art = candidate('aspect', width, height)
    pool.add([art], 'winter')
    expected = art if allowed else None
    assert pick(pool) == expected
    pool.mark_shown(art, NOW)
    assert pick(pool) == expected


def test_art_selection_retries_after_decoding_unknown_dimensions(pool, monkeypatch):
    from ventanal import jobs

    cfg = load_config(EXAMPLE)
    season = season_for(NOW.date(), cfg.seasons).name
    unknown = replace(candidate('unknown', None, None), kind='painting')
    fallback = replace(candidate('fallback', 3000, 1000), kind='painting')
    pool.add([unknown], season)
    pool.add([fallback], 'spring')
    fetched = []

    def fetch(c, client, cfg):
        fetched.append(c.object_id)
        return Image.new('RGB', (100, 301) if c == unknown else (300, 100))

    monkeypatch.setattr(jobs, 'fetch_candidate', fetch)
    picked, image = jobs.select_artwork(cfg, NOW, None, pool)
    assert fetched == ['unknown', 'fallback']
    assert picked == fallback
    assert image.size == (300, 100)


def test_quality_in_any_season_fallback_and_nonempty_last_resort(pool):
    small, large = candidate('small', 1200, 900), candidate('large')
    pool.add([small], 'winter')
    pool.add([large], 'autumn')
    assert pick(pool) == large
    pool.mark_shown(large, NOW)
    assert pick(pool) == small


@pytest.mark.parametrize('width,height', [(None, None), (1200, None), (None, 900), (1600, 900)])
def test_unknown_size_and_exact_minimum_eligible(pool, width, height):
    art = candidate('unknown', width, height)
    pool.add([art], 'winter')
    pool.add([candidate('other')], 'autumn')
    assert pick(pool) == art


def test_landscape_weighting(pool):
    landscape = candidate('landscape')
    portrait = candidate('portrait', 1500, 2000)
    pool.add([landscape, portrait], 'winter')
    rng = random.Random(2026)
    count = sum(pick(pool, rng=rng) == landscape for _ in range(2000))
    assert 1400 < count < 1600


def test_dedup_membership_and_persistence(tmp_path):
    path = tmp_path / 'pool.sqlite3'
    pool = ArtPool(path)
    art = candidate('1')
    assert pool.add(iter([art, art]), 'winter') == 1
    assert pool.add([art], 'spring') == 0
    other_source = replace(art, source='aic')
    assert pool.add([other_source], 'autumn') == 1
    reopened = ArtPool(path)
    assert pick(reopened, season='spring') == art
    reopened.mark_shown(art, NOW)
    assert pool.add([art], 'winter') == 0
    assert pick(ArtPool(path)) == other_source


def test_timestamps_compare_instants_across_offsets(pool):
    first, second = candidate('1'), candidate('2')
    pool.add([first, second], 'winter')
    # Lexical ISO sorting would incorrectly put first before second.
    pool.mark_shown(first, datetime.fromisoformat('2026-11-15T08:00:00-05:00'))
    pool.mark_shown(second, datetime.fromisoformat('2026-11-15T12:30:00+00:00'))
    assert pick(pool, now=NOW + timedelta(hours=2)) == second


def test_refresh_timestamp_persists(tmp_path):
    path = tmp_path / 'art.sqlite3'
    pool = ArtPool(path)
    assert pool.last_refresh() is None
    refreshed = datetime.fromisoformat('2026-11-15T08:00:00-05:00')
    pool.set_refreshed(refreshed)
    assert ArtPool(path).last_refresh() == refreshed
    assert ArtPool(path).last_refresh().utcoffset() == refreshed.utcoffset()


def test_naive_datetimes_rejected(pool):
    art = candidate('1')
    pool.add([art], 'winter')
    naive = NOW.replace(tzinfo=None)
    with pytest.raises(ValueError, match='timezone-aware'):
        pool.mark_shown(art, naive)
    with pytest.raises(ValueError, match='timezone-aware'):
        pick(pool, now=naive)
    with pytest.raises(ValueError, match='timezone-aware'):
        pool.set_refreshed(naive)


@pytest.mark.parametrize('today,current,next_name', [
    (date(2026, 11, 15), 'holiday-winter', 'winter'),
    (date(2026, 4, 1), 'spring', 'summer'),
])
def test_refresh_current_and_next_season(pool, monkeypatch, today, current, next_name):
    cfg = load_config(EXAMPLE)
    cfg = replace(cfg, seasons=tuple(reversed(cfg.seasons)), museums=('aic', 'met'))
    calls = []
    shared = candidate('shared')

    def search(sources, terms, limit_per_term, client, *, unavailable):
        calls.append((list(sources), tuple(terms), limit_per_term, client))
        return [shared, candidate(str(len(calls)))]

    monkeypatch.setattr('ventanal.art.pool.search_all', search)
    before = datetime.now(timezone.utc)
    with httpx.Client() as client:
        assert refresh_pool(cfg, pool, client, today) == 3
        assert len(calls) == 2
        for call, name in zip(calls, (current, next_name)):
            season = next(s for s in cfg.seasons if s.name == name)
            assert call == ([SOURCES['aic'], SOURCES['met']], season.terms, 40, client)
    assert before <= pool.last_refresh() <= datetime.now(timezone.utc)
    # Shared artwork belongs to both seasons, with a single global shown time.
    pool.mark_shown(candidate('1'), NOW)
    pool.mark_shown(candidate('2'), NOW)
    assert pick(pool, season=current) == shared
    assert pick(pool, season=next_name) == shared


def test_refresh_wrapping_season_skips_overlapping_starts(pool, monkeypatch):
    cfg = replace(load_config(EXAMPLE), seasons=(
        Season('overlap', (1, 1), (2, 28), ('overlap',)),
        Season('winter', (12, 1), (2, 28), ('snow',)),
        Season('spring', (3, 1), (11, 30), ('flowers',)),
    ))
    calls = []
    monkeypatch.setattr('ventanal.art.pool.search_all',
                        lambda sources, terms, limit_per_term, client, *, unavailable: calls.append(tuple(terms)) or [])
    with httpx.Client() as client:
        assert refresh_pool(cfg, pool, client, date(2026, 12, 15)) == 0
    assert calls == [('snow',), ('flowers',)]
    assert pool.last_refresh() is None


def test_stale_refresh_retries_after_one_hour_until_candidates_return(pool, monkeypatch):
    from ventanal.jobs import ensure_pool

    cfg = load_config(EXAMPLE)
    older = NOW - timedelta(days=2)
    pool.set_refreshed(older)
    artwork = candidate('existing')
    pool.add([artwork], 'holiday-winter')
    results = iter([[], [], [artwork], [artwork]])
    monkeypatch.setattr('ventanal.art.pool.search_all', lambda *a, **k: next(results))
    retry = NOW + timedelta(hours=1)
    with httpx.Client() as client:
        assert ensure_pool(cfg, pool, client, NOW) == 0
        assert pool.last_refresh() == older
        assert pool.retry_after() == retry
        assert ensure_pool(cfg, pool, client, retry - timedelta(seconds=1)) == 0
        assert pool.last_refresh() == older
        # Duplicates still prove the sources answered with candidates.
        assert ensure_pool(cfg, pool, client, retry) == 0
        assert pool.last_refresh() == retry
        assert pool.retry_after() is None
        assert ensure_pool(cfg, pool, client, retry) == 0


def test_refresh_download_metadata_preserves_shown_history(pool):
    art = candidate('1')
    pool.add([art], 'winter')
    pool.mark_shown(art, NOW)
    updated = replace(art, image_url='https://example.test/full/1686', width=1686, height=1200)
    assert pool.add([updated], 'spring') == 0
    other = candidate('2')
    pool.add([other], 'winter')
    assert pick(pool, season='winter') == other
    assert pick(pool, season='spring', now=NOW + timedelta(days=100)) == updated
