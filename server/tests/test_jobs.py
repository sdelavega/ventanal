# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Stephen de la Vega

from contextlib import closing
from dataclasses import replace
from datetime import datetime, time, timedelta
import json
import sqlite3
from pathlib import Path
from zoneinfo import ZoneInfo

import httpx
import numpy as np
from PIL import Image
import pytest

from ventanal import jobs
from ventanal.config import load_config
from ventanal.art.sources import Candidate
from ventanal.manifest import read_manifest

TZ = ZoneInfo('America/New_York')
NOW = datetime(2026, 11, 15, 5, 55, tzinfo=TZ)


@pytest.mark.parametrize(('hour', 'minute', 'day', 'slot'), [(5, 56, 15, 6), (5, 54, 15, 2), (0, 30, 14, 22)])
def test_current_slot(hour, minute, day, slot):
    assert jobs.current_slot(NOW.replace(hour=hour, minute=minute), [time(h) for h in (2, 6, 10, 14, 18, 22)]) == NOW.replace(day=day, hour=slot, minute=0)


@pytest.fixture
def setup_job(tmp_path, monkeypatch):
    cfg = replace(load_config(Path('ventanal.example.toml')), output_dir=tmp_path / 'out', cache_dir=tmp_path / 'cache')
    cfg.cache_dir.mkdir()
    pool = jobs.ArtPool(cfg.cache_dir / 'art.sqlite3')
    pool.add([Candidate('met', '1', 'Snow', 'Artist', '1900', 'Met', 'https://example.test/art', 1800, 1200, kind='painting')], 'holiday-winter')
    pool.set_refreshed(NOW)
    monkeypatch.setattr(jobs, 'fetch_image', lambda *a: Image.new('RGB', (16, 12)))
    monkeypatch.setattr(jobs, 'render_art', lambda *a: (object(), {'title': 'Snow'}))
    monkeypatch.setattr(jobs, 'quantize', lambda *a: np.zeros((1200, 1600), dtype=np.uint8))
    monkeypatch.setattr(jobs, 'simulate', lambda *a: Image.new('RGB', (16, 12)))
    monkeypatch.setattr(jobs, 'geocode', lambda *a: (29.9144193, -81.3628995, 'ST. AUGUSTINE · FL'))
    monkeypatch.setattr(jobs, 'fetch_weather', lambda *a, **k: object())
    monkeypatch.setattr(jobs, 'fetch_alerts', lambda *a: [])
    monkeypatch.setattr(jobs, 'fetch_iem', lambda *a: None)
    monkeypatch.setattr(jobs, 'basemap', lambda *a: {'states': [], 'counties': []})
    monkeypatch.setattr(jobs, 'render_weather', lambda *a: object())
    return cfg


def test_tick_publishes_art_once_per_slot(setup_job):
    with httpx.Client() as client:
        assert jobs.tick(setup_job, NOW, client) == {'art': True, 'weather': True, 'pool': 0}
        assert jobs.tick(setup_job, NOW + timedelta(minutes=30), client) == {'art': False, 'weather': True, 'pool': 0}
    state = json.loads((setup_job.cache_dir / 'state.json').read_text())
    assert state['last_art_slot'] == NOW.replace(hour=6, minute=0).isoformat()


def test_weather_failure_keeps_old_entry(setup_job, monkeypatch):
    with httpx.Client() as client:
        jobs.tick(setup_job, NOW, client)
        old = read_manifest(setup_job.output_dir)['screens']['weather']
        def fail(*a, **k):
            raise httpx.ConnectError('offline')
        monkeypatch.setattr(jobs, 'fetch_weather', fail)
        assert jobs.tick(setup_job, NOW + timedelta(minutes=30), client)['weather'] is False
    assert read_manifest(setup_job.output_dir)['screens']['weather'] == old


@pytest.mark.parametrize('dependency', ['fetch_alerts', 'fetch_iem', 'basemap'])
def test_optional_weather_failure_still_publishes(setup_job, monkeypatch, dependency):
    def fail(*a, **k):
        raise httpx.ConnectError('offline')
    monkeypatch.setattr(jobs, dependency, fail)
    with httpx.Client() as client:
        assert jobs.tick(setup_job, NOW, client)['weather'] is True


def test_weather_interval_and_pool_refresh(setup_job, monkeypatch):
    calls = []
    monkeypatch.setattr(jobs, 'refresh_pool', lambda *a, **kw: calls.append(a) or 7)
    with httpx.Client() as client:
        jobs.tick(setup_job, NOW, client)
        assert jobs.tick(setup_job, NOW + timedelta(minutes=24), client)['weather'] is False
        assert jobs.tick(setup_job, NOW + timedelta(minutes=25), client)['weather'] is True
        assert jobs.tick(setup_job, NOW + timedelta(hours=25), client)['pool'] == 7
    assert len(calls) == 1


def test_art_failure_retries_same_slot(setup_job, monkeypatch):
    def fail(*a):
        raise httpx.ConnectError('offline')
    original = jobs.fetch_image
    monkeypatch.setattr(jobs, 'fetch_image', fail)
    with httpx.Client() as client:
        assert jobs.tick(setup_job, NOW, client)['art'] is False
        monkeypatch.setattr(jobs, 'fetch_image', original)
        assert jobs.tick(setup_job, NOW, client)['art'] is True


def test_cli_commands_and_config_placement(setup_job, monkeypatch, capsys):
    from ventanal import cli
    loaded = []
    monkeypatch.setattr(cli, 'load_config', lambda path: loaded.append(path) or setup_job)
    monkeypatch.setattr(cli.jobs, 'tick', lambda *a, **kw: {'art': True, 'weather': True, 'pool': 0})
    cli.main(['--config', 'before.toml', 'tick'])
    cli.main(['tick', '--config', 'after.toml'])
    assert loaded == [Path('before.toml'), Path('after.toml')]
    assert json.loads(capsys.readouterr().out.splitlines()[0])['art'] is True
    for command in ('calibrate',):
        with pytest.raises(SystemExit) as error:
            cli.main([command])
        assert error.value.code == 2


@pytest.mark.parametrize('command', ['render-art', 'render-weather'])
def test_cli_preview_does_not_publish(setup_job, monkeypatch, command, tmp_path):
    from ventanal import cli
    monkeypatch.setattr(cli, 'load_config', lambda path: setup_job)
    monkeypatch.setattr(cli.jobs, 'art_canvas', lambda *a: (object(), {}, None))
    monkeypatch.setattr(cli.jobs, 'weather_canvas', lambda *a: object())
    monkeypatch.setattr(cli.jobs, 'refresh_pool', lambda *a, **kw: 0)
    monkeypatch.setattr(cli, 'quantize', lambda *a: np.zeros((1200, 1600), dtype=np.uint8))
    monkeypatch.setattr(cli, 'simulate', lambda *a: Image.new('RGB', (16, 12)))
    destination = tmp_path / f'{command}.png'
    cli.main([command, '--out', str(destination)])
    with Image.open(destination) as preview:
        assert preview.format == 'PNG'
    assert not (setup_job.cache_dir / 'state.json').exists()
    assert not (setup_job.output_dir / 'manifest.json').exists()


def test_mark_shown_follows_art_publish(setup_job, monkeypatch):
    shown = []
    original = jobs.ArtPool.mark_shown
    def mark(pool, candidate, now):
        assert 'art' in read_manifest(setup_job.output_dir)['screens']
        shown.append(candidate.object_id)
        return original(pool, candidate, now)
    monkeypatch.setattr(jobs.ArtPool, 'mark_shown', mark)
    with httpx.Client() as client:
        jobs.tick(setup_job, NOW, client)
        jobs.tick(setup_job, NOW, client)
    assert shown == ['1']


def test_radar_none_and_failed_optional_inputs(setup_job, monkeypatch):
    setup_job = replace(setup_job, radar='none')
    def unexpected(*a):
        pytest.fail('radar must not be fetched when disabled')
    monkeypatch.setattr(jobs, 'fetch_iem', unexpected)
    def fail(*a):
        raise httpx.ConnectError('offline')
    monkeypatch.setattr(jobs, 'fetch_alerts', fail)
    monkeypatch.setattr(jobs, 'basemap', fail)
    captured = []
    monkeypatch.setattr(jobs, 'render_weather', lambda *a: captured.append(a) or object())
    with httpx.Client() as client:
        assert jobs.tick(setup_job, NOW, client)['weather'] is True
    assert captured[0][1:4] == ([], None, {'states': [], 'counties': []})


def test_naive_time_rejected(setup_job):
    with pytest.raises(ValueError, match='timezone-aware'):
        jobs.current_slot(NOW.replace(tzinfo=None), setup_job.art_slots)
    with httpx.Client() as client, pytest.raises(ValueError, match='timezone-aware'):
        jobs.tick(setup_job, NOW.replace(tzinfo=None), client)


def test_cli_pool_refresh_shared_and_bookkept(setup_job, monkeypatch, tmp_path):
    from ventanal import cli
    from ventanal.art.sources import USER_AGENT
    monkeypatch.setattr(cli, 'load_config', lambda path: setup_job)
    calls = []
    def search(sources, terms, limit_per_term, client, *, unavailable):
        assert client.headers['User-Agent'] == USER_AGENT
        calls.append(terms)
        return [Candidate('met', '123', 'Snow', 'Artist', '1900', 'Met',
                          'https://example.test/art', 1800, 1200)]
    monkeypatch.setattr('ventanal.art.pool.search_all', search)
    monkeypatch.setattr(jobs, 'art_canvas', lambda *a: (object(), {}, None))
    monkeypatch.setattr(cli, 'quantize', lambda *a: np.zeros((12, 16), dtype=np.uint8))
    monkeypatch.setattr(cli, 'simulate', lambda *a: Image.new('RGB', (16, 12)))
    pool = jobs.ArtPool(setup_job.cache_dir / 'art.sqlite3')
    pool.set_refreshed(NOW - timedelta(days=10))
    for _ in range(2):
        cli.main(['render-art', '--out', str(tmp_path / 'preview.png')])
    assert len(calls) == 2  # Current and upcoming seasons, only on the first call.
    assert pool.last_refresh() > NOW
    assert not (setup_job.cache_dir / 'state.json').exists()
    with httpx.Client() as client:
        assert jobs.ensure_pool(setup_job, pool, client, pool.last_refresh()) == 0


@pytest.mark.parametrize('content', ['{broken', '[]'])
def test_unreadable_state_recovers(setup_job, caplog, content):
    (setup_job.cache_dir / 'state.json').write_text(content)
    with httpx.Client() as client:
        assert jobs.tick(setup_job, NOW, client)['weather'] is True
    assert 'state' in caplog.text and 'WARNING' in caplog.text


def test_state_read_oserror_recovers(setup_job, monkeypatch, caplog):
    original = Path.read_text
    def read(path, *args, **kwargs):
        if path.name == 'state.json':
            raise PermissionError('unreadable')
        return original(path, *args, **kwargs)
    monkeypatch.setattr(Path, 'read_text', read)
    with httpx.Client() as client:
        assert jobs.tick(setup_job, NOW, client)['weather'] is True
    assert 'state' in caplog.text and 'WARNING' in caplog.text


@pytest.mark.parametrize('minutes', [1, 241])
def test_force_art_reuses_published_candidate_without_rotation(setup_job, monkeypatch, minutes):
    cfg = replace(setup_job, weather_interval_minutes=10000)
    from dataclasses import asdict
    candidate = Candidate('met', '1', 'Snow', 'Artist', '1900', 'Met', 'https://example.test/art', 1800, 1200, kind='painting')
    monkeypatch.setattr(jobs, 'render_art', lambda image, c: (object(), {'title': c.title, 'source': c.source, 'object_id': c.object_id}))
    with httpx.Client() as client:
        jobs.tick(cfg, NOW, client)
        state = (cfg.cache_dir / 'state.json').read_bytes()
        weather = read_manifest(cfg.output_dir)['screens']['weather']
        def forbidden(*a, **k):
            pytest.fail('force-art must not select or mark artwork')
        monkeypatch.setattr(jobs.ArtPool, 'pick_next', forbidden)
        monkeypatch.setattr(jobs.ArtPool, 'mark_shown', forbidden)
        seen = []
        monkeypatch.setattr(jobs, 'fetch_image', lambda url, *a: seen.append(url) or Image.new('RGB', (16, 12)))
        assert jobs.tick(cfg, NOW + timedelta(minutes=minutes), client, force_art=True)['art'] is True
    assert seen == [candidate.image_url]
    assert (cfg.cache_dir / 'state.json').read_bytes() == state
    manifest = read_manifest(cfg.output_dir)
    assert manifest['screens']['weather'] == weather
    assert manifest['screens']['art']['readout'] == asdict(jobs.ART_READOUT)
    assert manifest['screens']['art']['credit']['object_id'] == '1'


def test_force_art_without_current_art_does_not_pick(setup_job, monkeypatch):
    monkeypatch.setattr(jobs, 'art_canvas', lambda *a: pytest.fail('must not pick new artwork'))
    with httpx.Client() as client:
        assert jobs.tick(setup_job, NOW, client, force_art=True)['art'] is False


@pytest.mark.parametrize('force_art', [False, True])
def test_cli_force_art(setup_job, monkeypatch, capsys, force_art):
    from ventanal import cli
    monkeypatch.setattr(cli, 'load_config', lambda path: setup_job)
    seen = []
    monkeypatch.setattr(jobs, 'tick', lambda *a, **kw: seen.append(kw) or {})
    cli.main(['tick'] + (['--force-art'] if force_art else []))
    assert seen == [{'force_art': force_art, 'force_spares': False, 'force_weather': False}]


def test_force_art_failure_preserves_published_art_and_state(setup_job, monkeypatch):
    cfg = setup_job
    monkeypatch.setattr(jobs, 'render_art', lambda image, c: (object(), {'source': c.source, 'object_id': c.object_id}))
    with httpx.Client() as client:
        jobs.tick(cfg, NOW, client)
        previous = read_manifest(cfg.output_dir)
        state = (cfg.cache_dir / 'state.json').read_bytes()
        def fail(*a, **k):
            raise httpx.ConnectError('offline')
        monkeypatch.setattr(jobs, 'fetch_image', fail)
        assert jobs.tick(cfg, NOW, client, force_art=True)['art'] is False
    assert read_manifest(cfg.output_dir) == previous
    assert (cfg.cache_dir / 'state.json').read_bytes() == state


def test_force_art_removed_from_pool_preserves_publication(setup_job, monkeypatch, caplog):
    cfg = setup_job
    monkeypatch.setattr(jobs, 'render_art', lambda image, c: (object(), {'source': c.source, 'object_id': c.object_id}))
    with httpx.Client() as client:
        jobs.tick(cfg, NOW, client)
        manifest = (cfg.output_dir / 'manifest.json').read_bytes()
        frame_path = cfg.output_dir / json.loads(manifest)['screens']['art']['url'].lstrip('/')
        frame = frame_path.read_bytes()
        state = (cfg.cache_dir / 'state.json').read_bytes()
        with closing(sqlite3.connect(cfg.cache_dir / 'art.sqlite3')) as db, db:
            db.execute('DELETE FROM artworks')
        def forbidden(*a, **kw):
            pytest.fail('missing published artwork must not fetch, select or mark artwork')
        monkeypatch.setattr(jobs, 'fetch_image', forbidden)
        monkeypatch.setattr(jobs.ArtPool, 'pick_next', forbidden)
        monkeypatch.setattr(jobs.ArtPool, 'mark_shown', forbidden)
        assert jobs.tick(cfg, NOW + timedelta(minutes=1), client, force_art=True)['art'] is False
    assert 'published artwork is absent from the pool' in caplog.text
    assert (cfg.output_dir / 'manifest.json').read_bytes() == manifest
    assert frame_path.read_bytes() == frame
    assert (cfg.cache_dir / 'state.json').read_bytes() == state


def test_manifest_quiet_hours(setup_job):
    cfg = replace(setup_job, quiet_start='12:34', quiet_end='12:34')
    with httpx.Client() as client:
        jobs.tick(cfg, NOW, client)
    assert read_manifest(cfg.output_dir)['quiet'] == {'start': '12:34', 'end': '12:34'}


def test_force_weather_before_due(setup_job):
    with httpx.Client() as client:
        jobs.tick(setup_job, NOW, client)
        assert jobs.tick(setup_job, NOW + timedelta(minutes=1), client,
                         force_weather=True)['weather'] is True
    state = json.loads((setup_job.cache_dir / 'state.json').read_text())
    assert state['last_weather'] == (NOW + timedelta(minutes=1)).isoformat()


def test_cli_force_weather(setup_job, monkeypatch, capsys):
    from ventanal import cli
    seen = []
    monkeypatch.setattr(cli, 'load_config', lambda *a: setup_job)
    monkeypatch.setattr(cli.jobs, 'tick', lambda *a, **kw: seen.append(kw) or {})
    cli.main(['tick', '--force-weather'])
    assert seen == [{'force_art': False, 'force_spares': False, 'force_weather': True}]


def test_tick_uses_one_board_store(setup_job, monkeypatch):
    from ventanal.status import BoardStore
    calls = []
    def create(directory):
        calls.append(directory)
        return BoardStore(directory)
    monkeypatch.setattr('ventanal.status.BoardStore', create)
    with httpx.Client() as client:
        jobs.tick(setup_job, NOW, client)
    assert calls == [setup_job.cache_dir]
