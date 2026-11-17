# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Stephen de la Vega

from dataclasses import FrozenInstanceError
from hashlib import sha256
from io import BytesIO
import json
from pathlib import Path

import httpx
from PIL import Image
import pytest

from ventanal.art.sources import (
    AICSource, Candidate, ClevelandSource, MetSource, SOURCES, fetch_image, search_all,
)


FIXTURES = Path(__file__).parent / 'fixtures'
USER_AGENT = 'Ventanal/1.0.0'


def fixture(name):
    return json.loads((FIXTURES / name).read_text())


def museum_transport(*, met_overrides=None, fail_aic=False, requests=None):
    def respond(request):
        assert request.headers['User-Agent'] == USER_AGENT
        if requests is not None:
            requests.append(request)
        if request.url.host == 'collectionapi.metmuseum.org':
            if request.url.path.endswith('/search'):
                assert request.url.path == '/public/collection/v1.1/search'
                assert request.url.params['hasImages'] == 'true'
                assert request.url.params['offset'] == '0'
                data = fixture('met_search.json')
            else:
                data = fixture('met_object.json')
                data.update(met_overrides or {})
            return httpx.Response(200, json=data)
        if request.url.host == 'api.artic.edu':
            if fail_aic:
                raise httpx.ConnectError('museum offline', request=request)
            assert request.url.params['query[term][is_public_domain]'] == 'true'
            assert request.url.params['fields'] == (
                'id,title,artist_title,date_display,image_id,thumbnail,classification_title,is_public_domain,artwork_type_title,medium_display'
            )
            return httpx.Response(200, json=fixture('aic_search.json'))
        assert request.url.host == 'openaccess-api.clevelandart.org'
        assert request.url.params['cc0'] == '1'
        assert request.url.params['has_image'] == '1'
        return httpx.Response(200, json=fixture('cleveland_search.json'))
    return httpx.MockTransport(respond)


def test_met_parses():
    assert_parses(MetSource(), 'The Metropolitan Museum of Art')


def test_aic_parses():
    assert_parses(AICSource(), 'Art Institute of Chicago')


def test_cleveland_parses():
    assert_parses(ClevelandSource(), 'Cleveland Museum of Art')


def assert_parses(source, museum):
    with httpx.Client(transport=museum_transport()) as client:
        result = source.search('landscape', 1, client)
    assert len(result) == 1
    candidate = result[0]
    assert isinstance(candidate, Candidate)
    assert candidate.source == source.name
    assert candidate.object_id
    assert candidate.title
    assert candidate.image_url.startswith('https://')
    assert candidate.museum == museum
    with pytest.raises(FrozenInstanceError):
        candidate.title = 'changed'


def test_non_public_domain_dropped():
    with httpx.Client(transport=museum_transport(met_overrides={'isPublicDomain': False})) as client:
        assert MetSource().search('landscape', 3, client) == []


@pytest.mark.parametrize('overrides', [{'primaryImage': ''}, {'classification': 'Sculpture'}])
def test_met_filters(overrides):
    with httpx.Client(transport=museum_transport(met_overrides=overrides)) as client:
        assert MetSource().search('landscape', 3, client) == []


def test_one_source_failing(caplog):
    with httpx.Client(transport=museum_transport(fail_aic=True)) as client:
        result = search_all(SOURCES.values(), ['landscape'], 1, client)
    assert {candidate.source for candidate in result} == {'met', 'cleveland'}
    assert any(record.name == 'ventanal.art' and 'aic' in record.message for record in caplog.records)


def test_search_all_reuses_term_iterator_and_continues_after_failed_term(caplog):
    requests = []

    def respond(request):
        if request.url.params.get('q') == 'broken':
            raise httpx.ConnectError('term failed', request=request)
        return museum_transport(requests=requests).handle_request(request)

    with httpx.Client(transport=httpx.MockTransport(respond)) as client:
        result = search_all(SOURCES.values(), iter(['broken', 'landscape']), 1, client)
    assert {candidate.source for candidate in result} == {'met', 'aic', 'cleveland'}
    assert len(caplog.records) == 3


def test_candidate_metadata():
    with httpx.Client(transport=museum_transport()) as client:
        aic = AICSource().search('landscape', 1, client)[0]
        cleveland = ClevelandSource().search('landscape', 1, client)[0]
        met = MetSource().search('landscape', 1, client)[0]
    a = fixture('aic_search.json')['data'][0]
    c = fixture('cleveland_search.json')['data'][0]
    m = fixture('met_object.json')
    assert (met.artist, met.date, met.width, met.height) == (m['artistDisplayName'], m['objectDate'], None, None)
    assert (aic.artist, aic.date, aic.width, aic.height) == (a['artist_title'], a['date_display'], 1686, round(a['thumbnail']['height'] * 1686 / a['thumbnail']['width']))
    assert aic.image_url == f"https://www.artic.edu/iiif/2/{a['image_id']}/full/1686,/0/default.jpg"
    assert (cleveland.artist, cleveland.date) == (c['creators'][0]['description'], c['creation_date'])
    assert cleveland.image_url == c['images']['print']['url']
    assert (cleveland.width, cleveland.height) == (int(c['images']['print']['width']), int(c['images']['print']['height']))


@pytest.mark.parametrize('source', [MetSource(), AICSource(), ClevelandSource()])
def test_nonpositive_limit_makes_no_requests(source):
    def unexpected(request):
        pytest.fail('No request expected')
    with httpx.Client(transport=httpx.MockTransport(unexpected)) as client:
        assert source.search('landscape', 0, client) == []
        assert source.search('landscape', -1, client) == []


def test_met_limits_object_requests():
    requests = []
    with httpx.Client(transport=museum_transport(requests=requests)) as client:
        MetSource().search('landscape', 2, client)
    assert len(requests) == 3
    assert requests[0].url.params['limit'] == '2'


@pytest.mark.parametrize('source,name', [(AICSource(), 'aic_search.json'), (ClevelandSource(), 'cleveland_search.json')])
def test_missing_image_skipped_and_optional_metadata(source, name):
    data = fixture(name)
    data['data'] = data['data'][:2]
    first, second = data['data']
    if source.name == 'aic':
        first['image_id'] = None
        second.update(artist_title=None, date_display=None, thumbnail=None)
    else:
        first['images'] = {}
        second.update(creators=[], creation_date=None)
        second['images'].pop('print', None)
    with httpx.Client(transport=httpx.MockTransport(lambda request: httpx.Response(200, json=data))) as client:
        result = source.search('landscape', 2, client)
    assert len(result) == 1
    assert result[0].artist == ''
    assert result[0].date == ''
    if source.name == 'aic':
        assert result[0].width is None and result[0].height is None
    else:
        assert result[0].image_url == second['images']['web']['url']


def image_bytes():
    data = BytesIO()
    Image.new('RGB', (4, 3), 'red').save(data, format='PNG')
    return data.getvalue()


def test_fetch_image_caches_by_url(tmp_path):
    requests = []
    def respond(request):
        assert request.headers['User-Agent'] == USER_AGENT
        requests.append(request)
        return httpx.Response(200, content=image_bytes(), headers={'Content-Type': 'image/png'})
    url = 'https://museum.example/art.png'
    cache = tmp_path / 'images'
    with httpx.Client(transport=httpx.MockTransport(respond)) as client:
        first = fetch_image(url, client, cache)
        second = fetch_image(url, client, cache)
    assert len(requests) == 1
    assert first.size == second.size == (4, 3)
    assert second.getpixel((0, 0)) == (255, 0, 0)
    assert {path.name for path in cache.iterdir()} == {
        sha256(url.encode()).hexdigest(), 'image-cache.lock', 'image-cache-valid.json'
    }


@pytest.mark.parametrize('status,content_type,content', [
    (503, 'image/png', image_bytes()),
    (200, 'text/html', b'<html>error</html>'),
    (200, 'text/html', image_bytes()),
    (200, 'image/png', b'broken image'),
])
def test_fetch_image_does_not_cache_failures(tmp_path, status, content_type, content):
    requests = []
    def respond(request):
        requests.append(request)
        if len(requests) == 1:
            return httpx.Response(status, content=content, headers={'Content-Type': content_type})
        return httpx.Response(200, content=image_bytes(), headers={'Content-Type': 'image/png'})
    with httpx.Client(transport=httpx.MockTransport(respond)) as client:
        with pytest.raises((httpx.HTTPStatusError, ValueError, OSError)):
            fetch_image('https://museum.example/image', client, tmp_path)
        assert list(tmp_path.iterdir()) == []
        assert fetch_image('https://museum.example/image', client, tmp_path).size == (4, 3)
    assert len(requests) == 2


def test_fetch_image_follows_redirect_with_user_agent(tmp_path):
    def respond(request):
        assert request.headers['User-Agent'] == USER_AGENT
        if request.url.path == '/redirect':
            return httpx.Response(302, headers={'Location': 'https://cdn.example/image'})
        return httpx.Response(200, content=image_bytes(), headers={'Content-Type': 'image/png'})
    with httpx.Client(transport=httpx.MockTransport(respond)) as client:
        assert fetch_image('https://museum.example/redirect', client, tmp_path).size == (4, 3)


def test_aic_identifies_api_and_image_requests(tmp_path):
    def respond(request):
        assert request.headers['AIC-User-Agent'] == USER_AGENT
        if request.url.host == 'api.artic.edu':
            return httpx.Response(200, json=fixture('aic_search.json'))
        assert '/full/1686,/' in request.url.path
        return httpx.Response(200, content=image_bytes(), headers={'Content-Type': 'image/png'})
    with httpx.Client(transport=httpx.MockTransport(respond)) as client:
        candidate = AICSource().search('winter', 1, client)[0]
        assert candidate.width == 1686
        assert candidate.height == round(4176 * 1686 / 5898)
        fetch_image(candidate.image_url, client, tmp_path)


def test_blocked_museum_warns_once_per_refresh(tmp_path, caplog):
    from dataclasses import replace
    from datetime import date
    from ventanal.config import load_config
    from ventanal.art.pool import ArtPool, refresh_pool
    calls = []
    def respond(request):
        calls.append(request)
        return httpx.Response(403)
    cfg = replace(load_config(Path('ventanal.example.toml')), museums=['met'])
    with httpx.Client(transport=httpx.MockTransport(respond)) as client:
        assert refresh_pool(cfg, ArtPool(tmp_path / 'art.sqlite3'), client, date(2026, 11, 15)) == 0
    assert len(calls) == 1
    assert len(caplog.records) == 1
    assert caplog.records[0].levelname == 'WARNING'
    assert caplog.records[0].exc_info is not None
