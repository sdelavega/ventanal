# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Stephen de la Vega
from dataclasses import replace
from hashlib import sha256
from pathlib import Path
import json
import os
import stat

import httpx
from PIL import Image
import pytest

from ventanal.manifest import read_manifest, write_manifest
from ventanal.status import BoardStore

@pytest.mark.parametrize('raw', ['{', '[]', 'null', '42', '{}', '{"screens":[]}', '{"screens":{}}', '{"version":1,"screens":{"art":null}}'])
def test_manifest_recovery(tmp_path, caplog, raw):
    (tmp_path/'manifest.json').write_text(raw)
    store = BoardStore(tmp_path/'cache')
    assert read_manifest(tmp_path, store=store) == {'version': 1, 'screens': {}}
    assert 'manifest' in caplog.text
    assert store.recent()[0]['kind'] == 'manifest recovery'
    write_manifest(tmp_path, {'version':1,'screens':{}})
    assert read_manifest(tmp_path) == {'version':1,'screens':{}}

def test_manifest_fsync_file_then_directory(tmp_path, monkeypatch):
    import ventanal.manifest as m
    calls=[]
    original=m.os.fsync
    def sync(fd):
        calls.append('dir' if stat.S_ISDIR(os.fstat(fd).st_mode) else 'file')
        original(fd)
    monkeypatch.setattr(m.os,'fsync',sync)
    write_manifest(tmp_path, {'version':1,'screens':{}})
    assert calls == ['file','dir']

def test_image_cache_global_lru_and_protection(tmp_path):
    from ventanal.art.image_cache import configure, prune
    from ventanal.config import load_config
    from ventanal.art.sources.base import Candidate
    from ventanal.owner import OwnerStore
    cfg=replace(load_config(Path(__file__).parents[1]/'ventanal.example.toml'),cache_dir=tmp_path,output_dir=tmp_path/'out',image_cache_bytes=1)
    owner=OwnerStore(tmp_path)
    names=[]
    for i in range(5):
        c=Candidate('met',str(i),'','','','',f'https://test/{i}',1,1)
        name=sha256(c.image_url.encode()).hexdigest(); names.append(name)
        directory=tmp_path if i%2==0 else tmp_path/'sources'/'images'
        directory.mkdir(parents=True,exist_ok=True)
        Image.new('RGB',(2,2)).save(directory/name,format='PNG')
        os.utime(directory/name,(i+1,i+1))
        if i==0: owner.register('main',c)
        if i==1: owner.put_spare(0,{'id':'spare'},c)
    write_manifest(cfg.output_dir,{'version':1,'screens':{'art':{'id':'main'}}})
    bad=tmp_path/('f'*64); bad.write_bytes(b'corrupt')
    configure(cfg); prune(tmp_path)
    assert (tmp_path/names[0]).exists()
    assert (tmp_path/'sources/images'/names[1]).exists()
    assert not bad.exists()
    assert not any(p.exists() for p in [tmp_path/names[2],tmp_path/'sources/images'/names[3],tmp_path/names[4]])

def test_image_cache_hit_refreshes_lru_and_corrupt_refetch(tmp_path):
    from ventanal.art.image_cache import prune
    from ventanal.art.sources.base import fetch_image
    from io import BytesIO
    content=BytesIO(); Image.new('RGB',(2,2),'red').save(content,format='PNG')
    urls=['https://test/a','https://test/b']
    paths=[tmp_path/sha256(u.encode()).hexdigest() for u in urls]
    for i,p in enumerate(paths): p.write_bytes(content.getvalue()); os.utime(p,(i+1,i+1))
    with httpx.Client(transport=httpx.MockTransport(lambda r: httpx.Response(200,content=content.getvalue(),headers={'content-type':'image/png'}))) as client:
        fetch_image(urls[0],client,tmp_path)
        prune(tmp_path,max_bytes=len(content.getvalue()))
        assert paths[0].exists() and not paths[1].exists()
        paths[0].write_bytes(b'broken')
        assert fetch_image(urls[0],client,tmp_path).getpixel((0,0)) == (255,0,0)

def test_cache_limit_config(tmp_path):
    from ventanal.config import load_config, ConfigError
    example=(Path(__file__).parents[1]/'ventanal.example.toml').read_text()
    p=tmp_path/'config.toml'
    p.write_text(example.replace('image_cache_bytes = 2147483648','image_cache_bytes = 1234'))
    assert load_config(p).image_cache_bytes == 1234
    p.write_text(example.replace('image_cache_bytes = 2147483648','image_cache_bytes = -1'))
    with pytest.raises(ConfigError): load_config(p)


def test_manifest_io_failure_is_reported(tmp_path, monkeypatch, caplog):
    store = BoardStore(tmp_path/'cache')
    original = Path.read_text
    def fail(path, *args, **kwargs):
        if path.name == 'manifest.json':
            raise PermissionError('injected')
        return original(path, *args, **kwargs)
    monkeypatch.setattr(Path, 'read_text', fail)
    assert read_manifest(tmp_path, store=store) == {'version':1,'screens':{}}
    assert store.recent()[0]['kind'] == 'manifest recovery'
    assert 'manifest.json unreadable' in caplog.text


def test_prune_removes_truncated_image_payload(tmp_path):
    from ventanal.art.image_cache import prune
    from io import BytesIO
    content=BytesIO(); Image.new('RGB',(100,100)).save(content,format='JPEG')
    p=tmp_path/('a'*64)
    p.write_bytes(content.getvalue()[:-30])
    prune(tmp_path)
    assert not p.exists()
