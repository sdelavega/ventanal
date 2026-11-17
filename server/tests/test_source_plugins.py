# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Stephen de la Vega
"""Registry, safe credentials and explicit rights admission regressions."""
import httpx
import pytest
from pathlib import Path
from ventanal.art.sources import registry, iiif_url
from ventanal.config import load_config
from ventanal.settings import save_settings


def test_registry():
    sources = registry()
    assert set(sources) == {'met','aic','cleveland','rijksmuseum','nga','paris','smk','commons'}
    assert not sources['commons'].default_enabled
    assert not sources['paris'].default_enabled
    assert sources['paris'].credentials[0].name == 'token'
    assert sources['rijksmuseum'].credentials == ()
    assert all(s.id == key and s.name and s.description and s.rate_limit > 0 for key,s in sources.items())


def test_iiif_size():
    assert iiif_url('https://iiif.example/id', 9000) == 'https://iiif.example/id/full/!3400,3400/0/default.jpg'


def test_config_migration_credentials(tmp_path):
    path = tmp_path/'settings.toml'
    path.write_text(Path('ventanal.example.toml').read_text())
    old = load_config(path)
    cfg = save_settings(path, {'art.museums':['rijksmuseum','nga','smk'], 'art.credentials.paris.token':'private-fixture-token'})
    assert old.museums == ('met','aic','cleveland')
    assert cfg.credentials['paris']['token'] == 'private-fixture-token'
    assert 'private-fixture-token' not in repr(cfg)
    assert save_settings(path, {'art.credentials.paris.token':''}).credentials == cfg.credentials
    assert save_settings(path, {'art.credentials.paris.token':'','art.credentials.paris.token.clear':'yes'}).credentials['paris']['token'] == ''


def test_credentials_failure_never_logged(caplog):
    from ventanal.art.sources import search_all
    source = registry(credentials={'paris':{'token':'private-fixture-token'}})['paris']
    with httpx.Client(transport=httpx.MockTransport(lambda r: httpx.Response(403))) as client:
        assert search_all([source], ['winter'], 3, client) == []
    assert 'private-fixture-token' not in caplog.text

FIXTURES = Path(__file__).parent/'fixtures'/'sources'


def sample(name):
    import json
    return json.loads((FIXTURES/(name+'.json')).read_text())


@pytest.mark.parametrize('ident,fixture', [('smk','smk'),('commons','commons')])
def test_recorded_search_and_rights(ident, fixture):
    import copy
    data = sample(fixture)
    source = registry()[ident]
    with httpx.Client(transport=httpx.MockTransport(lambda r: httpx.Response(200,json=data))) as client:
        result = source.search('winter', client, 3)
        assert result
        assert all(c.source == ident and source.attribution(c) for c in result)
        if ident == 'smk':
            assert all('/full/!2400,2400/' in c.image_url for c in result)
            for r in data['items']: r['public_domain'] = False
        else:
            for r in data['query']['pages'].values():
                for image in r.get('imageinfo',[]):
                    image['extmetadata']['LicenseShortName'] = {'value':'CC BY-SA 4.0'}
        assert source.search('winter', client, 3) == []


def test_rijks_uses_visual_rights_not_metadata_licence():
    obj = sample('rijks-object'); visual = sample('rijks-visual'); digital = sample('rijks-digital')
    source = registry()['rijksmuseum']
    def respond(r):
        if r.url.path.endswith('collection'): data = {'orderedItems':[{'id':obj['id']}]}
        elif r.url.path.endswith('200108396'): data = obj
        elif r.url.path.endswith('202108396'): data = visual
        elif r.url.path.endswith('info.json'): data = {'width':4000,'height':3000}
        else: data = digital
        return httpx.Response(200,json=data)
    with httpx.Client(transport=httpx.MockTransport(respond)) as client:
        result = source.search('autumn', client, 3)
        assert len(result)==1 and result[0].artist and result[0].title
        visual['subject_to'] = []
        assert source.search('autumn', client, 3) == []


def test_nga_weekly_cached_join_and_rights(tmp_path):
    source = registry(cache_dir=tmp_path)['nga']
    requests = []
    def respond(r):
        requests.append(r)
        name = 'nga-objects' if r.url.path.endswith('objects.csv') else 'nga-images'
        return httpx.Response(200,text=(FIXTURES/(name+'.csv')).read_text())
    with httpx.Client(transport=httpx.MockTransport(respond)) as client:
        assert source.search('winter', client, 3)
        assert len(requests)==2
        assert source.search('winter', client, 3)
        assert registry(cache_dir=tmp_path)['nga'].search('winter',client,3)
        assert len(requests)==2
    assert not source.rights_filter({'openaccess':'0'})
    assert not source.rights_filter({})


def test_smk_ids_fit_ignored_contract():
    import re
    source = registry()['smk']
    data = sample('smk'); data['items'][0]['object_number']='KKS1/2'
    with httpx.Client(transport=httpx.MockTransport(lambda r: httpx.Response(200,json=data))) as client:
        c = source.search('winter',client,1)[0]
    assert re.fullmatch('[A-Za-z0-9_-]+',c.object_id)


def test_commons_cc0_and_art_only():
    source=registry()['commons']
    image={'extmetadata': {'LicenseShortName':{'value':'CC0'},
        'UsageTerms':{'value':'Creative Commons Zero, Public Domain Dedication'},
        'Copyrighted':{'value':'True'}, 'Categories':{'value':'CC-Zero|Winter landscape paintings'}}}
    assert source.rights_filter(image)
    image['extmetadata']['Categories']={'value':'Photographs of people'}
    assert not source.rights_filter(image)


def test_settings_sources_and_no_credential_echo(tmp_path):
    from ventanal.ui import UI
    path=tmp_path/'settings.toml'; path.write_text(Path('ventanal.example.toml').read_text())
    save_settings(path,{'art.credentials.paris.token':'fixture-hidden-token'})
    ui=object.__new__(UI); ui.config_path=path; ui.render_result=''
    body=ui.settings_body('csrf', {'art.credentials.paris.token':'submitted-hidden-token'})
    assert 'fixture-hidden-token' not in body and 'submitted-hidden-token' not in body
    assert 'Rijksmuseum API key' not in body
    assert 'name="art.credentials.paris.token" type="password" value=""' in body
    assert 'Photos of public-domain paintings are treated as public domain in the US (PD-Art); some countries differ.' in body
    assert 'value="commons" checked' not in body
    assert 'value="rijksmuseum"' in body


def test_jobs_dispatch_new_source_fetch(tmp_path, monkeypatch):
    from ventanal import jobs
    from ventanal.art.sources import Candidate
    from dataclasses import replace
    cfg=replace(load_config(Path('ventanal.example.toml')),cache_dir=tmp_path)
    c=Candidate('smk','1','Snow','Artist','','SMK','https://image.test/a',1800,1200)
    calls=[]
    class Plugin:
        def fetch_image(self,candidate,client): calls.append(candidate); return 'image'
    monkeypatch.setattr('ventanal.art.sources.registry',lambda **kw:{'smk':Plugin()})
    assert jobs.fetch_candidate(c,httpx.Client(),cfg)=='image'
    assert calls==[c]


def test_paris_no_token_and_safe_errors(caplog):
    from ventanal.art.sources import search_all
    assert not registry()['paris'].rights_filter({'fieldOeuvreLibre':False})
    assert not registry()['paris'].rights_filter({})
    requests=[]
    def respond(r):
        requests.append(r)
        assert r.headers['auth-token']=='fixture-private'
        return httpx.Response(200,json={'errors':[{'message':'fixture-private'}]})
    with httpx.Client(transport=httpx.MockTransport(respond)) as client:
        assert registry()['paris'].search('winter',client,3)==[]
        assert not requests
        assert search_all([registry(credentials={'paris':{'token':'fixture-private'}})['paris']],['winter'],3,client)==[]
    assert 'fixture-private' not in caplog.text


def test_independent_protocol_plugin_search():
    from ventanal.art.sources import search_all
    calls=[]
    class Plugin:
        id='custom'; name='Custom collection'
        def search(self, season, client, limit):
            assert isinstance(client,httpx.Client)
            calls.append((season,limit)); return []
    with httpx.Client() as client:
        assert search_all([Plugin()],['winter'],3,client)==[]
    assert calls==[('winter',3)]
