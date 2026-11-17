# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Stephen de la Vega
from dataclasses import replace, asdict
from datetime import timedelta
import json
from pathlib import Path
import random
import httpx
import pytest
from test_jobs import setup_job, NOW
from ventanal import jobs, cli
from ventanal.art.pool import ArtPool
from ventanal.art.sources import Candidate
from ventanal.config import load_config, ConfigError
from ventanal.manifest import read_manifest
from ventanal.owner import OwnerStore, parse_skips


def test_ignored_config(tmp_path):
    example = Path('ventanal.example.toml').read_text()
    for value in ['["cleveland:173180"]', '[]']:
        path = tmp_path / 'test.toml'
        path.write_text(example.replace('ignored = []\n','').replace('[art]', f'[art]\nignored = {value}'))
        assert load_config(path).ignored == tuple(json.loads(value))
    for value in ['["bad"]', '["met:"]', '[1]', '"met:1"']:
        path.write_text(example.replace('ignored = []\n','').replace('[art]', f'[art]\nignored = {value}'))
        with pytest.raises(ConfigError, match='art.ignored'):
            load_config(path)


def test_pool_exclusions_include_fallback(tmp_path):
    pool = ArtPool(tmp_path / 'pool.db')
    c = Candidate('met','1','Title','Artist','1900','Met','url',1800,1200,kind='painting')
    pool.add([c], 'winter')
    pool.mark_shown(c, NOW)
    assert pool.pick_next('winter', NOW, 60, random.Random(), excluded={'met:1'}) is None
    assert pool.pick_next('winter', NOW, 60, random.Random(), strict=True) is None


def test_skip_parser(caplog):
    a,b='a'*16,'b'*16
    assert parse_skips(f'{a}~{b},no,{a}~bad') == [(a,b)]
    assert len(parse_skips(','.join([f'{a}~{b}']*9))) == 8
    assert 'malformed' in caplog.text and 'limit' in caplog.text


def populate(cfg, monkeypatch):
    pool=ArtPool(cfg.cache_dir / 'art.sqlite3')
    pool.add([Candidate('met',str(i),f'Title {i}','Artist','1900','Met',f'url{i}',1800,1200,kind='painting') for i in range(2,12)], 'holiday-winter')
    monkeypatch.setattr(jobs,'render_art',lambda image,c: (int(c.object_id),asdict(c)))
    import numpy as np
    monkeypatch.setattr(jobs,'quantize',lambda canvas,*a: np.full((1200,1600),canvas if isinstance(canvas,int) else 0,dtype=np.uint8))
    # Distinct encoded bytes for each artwork, independent of quantizer pixels.
    monkeypatch.setattr(jobs,'encode_frame',lambda indices: bytes(indices[:1,:1])+b'x'*16)
    return pool


def test_spares_persist_and_replace_only_reported(setup_job, monkeypatch):
    cfg=replace(setup_job, ignored=('met:2',), weather_interval_minutes=10000)
    pool=populate(cfg,monkeypatch)
    # Unique render IDs without depending on the six panel colours.
    monkeypatch.setattr(jobs,'encode_frame',lambda indices: indices.tobytes()[:1]+b'x'*16)
    with httpx.Client() as client:
        jobs.tick(cfg,NOW,client)
        manifest=read_manifest(cfg.output_dir)
        assert manifest['screens']['art']['credit']['object_id']!='2'
        assert len(manifest['spares'])==3
        before=manifest['spares']
        assert all(e['credit']['object_id']!='2' for e in before)
        jobs.tick(cfg,NOW+timedelta(hours=4),client)
        assert read_manifest(cfg.output_dir)['spares']==before
        store=OwnerStore(cfg.cache_dir)
        skipped=manifest['screens']['art']['id']; shown=before[0]['id']
        assert store.record(skipped,shown,NOW)
        assert not store.record(skipped,shown,NOW+timedelta(seconds=30))
        rows=store.skipped()
        assert len(rows)==1 and rows[0]['count']==1
        jobs.tick(cfg,NOW+timedelta(hours=4,minutes=1),client)
        after=read_manifest(cfg.output_dir)['spares']
        assert after[1:]==before[1:] and after[0]['id']!=before[0]['id']
        assert store.skipped()[0]['count']==1
        import sqlite3
        with sqlite3.connect(store.path) as db:
            row=db.execute('SELECT last_shown FROM artworks WHERE source=? AND object_id=?',
                           (before[0]['credit']['source'],before[0]['credit']['object_id'])).fetchone()
            assert row[0]==NOW.isoformat()
        assert not store.record('f'*16,shown,NOW)
        assert pool.get(before[0]['credit']['source'],before[0]['credit']['object_id']) is not None


def test_cli_skipped(setup_job,monkeypatch,capsys):
    store=OwnerStore(setup_job.cache_dir)
    c=Candidate('met','1','Snow','Artist','1900','Met','url',1800,1200,kind='painting')
    for i in range(3): store.register(str(i)*16,c)
    store.record('0'*16,'1'*16,NOW); store.record('0'*16,'2'*16,NOW)
    monkeypatch.setattr(cli,'load_config',lambda path: setup_job)
    cli.main(['skipped','--limit','1'])
    text=capsys.readouterr().out
    assert 'likely ignore' in text and '"met:1",  # Snow — Artist' in text and 'Met' in text
    monkeypatch.setattr(cli,'load_config',lambda path: replace(setup_job,ignored=('met:1',)))
    cli.main(['skipped','--limit','1'])
    assert '[ignored]' in capsys.readouterr().out


def test_http_skip_get_only_and_idempotent(tmp_path):
    from http.client import HTTPConnection
    from threading import Thread
    from ventanal.serve import make_server
    out=tmp_path/'out'; out.mkdir(); (out/'manifest.json').write_text('{"version":1}')
    cache=tmp_path/'cache'; store=OwnerStore(cache)
    c=Candidate('met','1','Snow','Artist','1900','Met','url',1800,1200,kind='painting')
    ArtPool(cache/'art.sqlite3').add([c],'winter')
    store.register('a'*16,c); store.register('b'*16,c)
    server=make_server(out,'127.0.0.1',0,cache_dir=cache)
    thread=Thread(target=server.serve_forever); thread.start()
    try:
        for method in ['HEAD','POST','GET','GET']:
            conn=HTTPConnection(*server.server_address)
            conn.request(method,'/manifest.json?skips='+('a'*16)+'~'+('b'*16)+',bad')
            response=conn.getresponse(); response.read(); conn.close()
            assert response.status==(200 if method=='GET' else 501)
            assert len(store.skipped())==(1 if method=='GET' else 0)
        assert store.skipped()[0]['count']==1
    finally:
        server.shutdown(); server.server_close(); thread.join()


def test_spare_render_failure_retries_without_removing_unused(setup_job,monkeypatch):
    populate(setup_job,monkeypatch)
    with httpx.Client() as client:
        jobs.tick(setup_job,NOW,client)
        before=read_manifest(setup_job.output_dir)['spares']
        store=OwnerStore(setup_job.cache_dir)
        store.record(read_manifest(setup_job.output_dir)['screens']['art']['id'],before[1]['id'],NOW)
        original=jobs.fetch_image
        def fail(*a): raise httpx.ConnectError('offline')
        monkeypatch.setattr(jobs,'fetch_image',fail)
        jobs.tick(setup_job,NOW+timedelta(minutes=1),client)
        assert read_manifest(setup_job.output_dir)['spares']==before
        monkeypatch.setattr(jobs,'fetch_image',original)
        jobs.tick(setup_job,NOW+timedelta(minutes=2),client)
        after=read_manifest(setup_job.output_dir)['spares']
        assert after[0]==before[0] and after[2]==before[2] and after[1]!=before[1]


def test_force_spares_retains_identity_and_history(setup_job, monkeypatch):
    populate(setup_job, monkeypatch)
    with httpx.Client() as client:
        jobs.tick(setup_job, NOW, client)
        before = read_manifest(setup_job.output_dir)
        state = (setup_job.cache_dir / 'state.json').read_text()
        store = OwnerStore(setup_job.cache_dir)
        store.record(before['screens']['art']['id'], before['spares'][1]['id'], NOW)
        rows = store.spares()
        with store.connect() as db:
            history = db.execute('SELECT source, object_id, last_shown FROM artworks ORDER BY source, object_id').fetchall()
        rendered = []
        original = jobs.render_art
        def render(image, candidate):
            rendered.append((candidate.source, candidate.object_id))
            return original(image, candidate)
        monkeypatch.setattr(jobs, 'render_art', render)
        jobs.tick(setup_job, NOW + timedelta(seconds=1), client,
                  force_art=True, force_spares=True)
        after = read_manifest(setup_job.output_dir)
        assert rendered == [(before['screens']['art']['credit']['source'],
                             before['screens']['art']['credit']['object_id'])] + [(r[2], r[3]) for r in rows]
        assert [e['credit'] for e in after['spares']] == [e['credit'] for e in before['spares']]
        assert all(e['rendered'] != b['rendered'] for e, b in zip(after['spares'], before['spares']))
        assert (setup_job.cache_dir / 'state.json').read_text() == state
        assert [(r[2],r[3],r[4]) for r in store.spares()] == [(r[2],r[3],r[4]) for r in rows]
        with store.connect() as db:
            assert db.execute('SELECT source, object_id, last_shown FROM artworks ORDER BY source, object_id').fetchall() == history
        def fail(*args):
            raise httpx.ConnectError('offline')
        monkeypatch.setattr(jobs, 'fetch_image', fail)
        jobs.tick(setup_job, NOW + timedelta(seconds=2), client,
                  force_art=True, force_spares=True)
        assert read_manifest(setup_job.output_dir)['spares'] == after['spares']


def test_cli_force_spares(setup_job, monkeypatch):
    monkeypatch.setattr(cli, 'load_config', lambda path: setup_job)
    calls = []
    def tick(*args, **kwargs):
        calls.append(kwargs)
        return {}
    monkeypatch.setattr(jobs, 'tick', tick)
    cli.main(['tick', '--force-art', '--force-spares'])
    assert calls == [{'force_art': True, 'force_spares': True, 'force_weather': False}]
