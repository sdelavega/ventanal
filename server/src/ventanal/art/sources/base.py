# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Stephen de la Vega

"""Adapters for public museum APIs and a validated image cache."""

from dataclasses import dataclass
from hashlib import sha256
from io import BytesIO
import os
from pathlib import Path
from tempfile import NamedTemporaryFile
from typing import Protocol
import threading
import time
import json

import httpx
from PIL import Image

from ventanal import USER_AGENT


@dataclass(frozen=True)
class Candidate:
    source: str
    object_id: str
    title: str
    artist: str
    date: str
    museum: str
    image_url: str
    width: int | None
    height: int | None
    kind: str = "other"


@dataclass(frozen=True)
class Credential:
    name: str
    label: str


class Source(Protocol):
    id: str
    name: str
    description: str
    default_enabled: bool
    credentials: tuple[Credential, ...]
    rate_limit: float

    def search(self, season, client: httpx.Client, limit: int) -> list[Candidate]: ...
    def fetch_image(self, candidate: Candidate, client: httpx.Client): ...
    def attribution(self, candidate: Candidate) -> str: ...
    def rights_filter(self, record: dict) -> bool: ...


MuseumSource = Source
_locks = {}
_last = {}
_guard = threading.Lock()


def _get(client, url, *, rate_limit=4, **kwargs):
    return request(client, 'GET', url, rate_limit=rate_limit, **kwargs)


def request(client, method, url, *, rate_limit=4, attempts=3, **kwargs):
    """Bounded retries; shared host pacing, including concurrent refreshes.

    Never include upstream exception text, request bodies or headers in logs.
    """
    host = httpx.URL(url).host
    headers = {'User-Agent': USER_AGENT}
    if host in ('api.artic.edu', 'www.artic.edu'):
        headers['AIC-User-Agent'] = USER_AGENT
    headers.update(kwargs.pop('headers', {}))
    with _guard:
        lock = _locks.setdefault(host, threading.Lock())
    for attempt in range(attempts):
        with lock:
            # MockTransport has no remote rate limit and should not slow tests.
            if not isinstance(client._transport, httpx.MockTransport):
                time.sleep(
                    max(0, _last.get(host, 0) + 1 / rate_limit - time.monotonic())
                )
            _last[host] = time.monotonic()
        try:
            response = client.request(
                method,
                url,
                headers=headers,
                follow_redirects='auth-token' not in headers,
                **kwargs,
            )
        except httpx.TransportError:
            if attempt == attempts - 1:
                raise
            time.sleep(0.25 * 2**attempt)
            continue
        if response.status_code in (429, 500, 502, 503, 504) and attempt < attempts - 1:
            try:
                delay = min(30, max(0, float(response.headers.get('Retry-After', 0))))
            except ValueError:
                delay = 0
            time.sleep(max(delay, 0.25 * 2**attempt))
            continue
        response.raise_for_status()
        return response


def iiif_url(service: str, size: int = 2400) -> str:
    size = min(3400, max(1600, size))
    return f'{service.rstrip("/")}/full/!{size},{size}/0/default.jpg'


def public_rights(value):
    return str(value).rstrip('/').replace('http:', 'https:') in {
        'https://creativecommons.org/publicdomain/zero/1.0',
        'https://creativecommons.org/publicdomain/mark/1.0',
    }


class BaseSource:
    default_enabled = True
    credentials = ()
    rate_limit = 4
    description = 'Public-domain art.'
    legal_note = ''

    def __init__(self, *, cache_dir=None, credentials=None):
        self.cache_dir = Path(cache_dir) / 'sources' if cache_dir else None
        self._credentials = credentials or {}

    def fetch_image(self, candidate, client):
        if self.cache_dir is None:
            raise ValueError('cache_dir is required to fetch images')
        return fetch_image(
            candidate.image_url,
            client,
            self.cache_dir / 'images',
            attempts=3,
            rate_limit=self.rate_limit,
        )

    def attribution(self, candidate):
        return ' · '.join(
            v
            for v in (
                candidate.title,
                candidate.artist,
                candidate.date,
                candidate.museum,
            )
            if v
        )

    def search(self, season, client, limit=40):
        # Compatibility for the original search(term, limit, client) API.
        if isinstance(client, int):
            client, limit = limit, client
        if limit <= 0:
            return []
        terms = (season,) if isinstance(season, str) else season.terms
        results = {}
        for term in terms:
            for c in self.search_term(term, limit, client):
                results[(c.source, c.object_id)] = c
                if len(results) >= limit:
                    return list(results.values())
        return list(results.values())

    def cached_json(self, key, client, url, *, max_age=86400, **kwargs):
        path = (
            self.cache_dir / (sha256(key.encode()).hexdigest() + '.json')
            if self.cache_dir
            else None
        )
        if path and path.is_file() and time.time() - path.stat().st_mtime < max_age:
            try:
                return json.loads(path.read_text())
            except (ValueError, OSError):
                pass
        data = _get(client, url, rate_limit=self.rate_limit, **kwargs).json()
        if path:
            path.parent.mkdir(parents=True, exist_ok=True)
            with NamedTemporaryFile(dir=path.parent, mode='w', delete=False) as output:
                json.dump(data, output)
                temporary = Path(output.name)
            os.replace(temporary, path)
        return data


def _dimension(value) -> int | None:
    return int(value) if value is not None else None


def fetch_image(
    url: str, client: httpx.Client, cache_dir: Path, *, attempts=1, rate_limit=4
) -> Image.Image:
    """Cache only successfully decoded images, publishing cache entries atomically."""
    path = cache_dir / sha256(url.encode('utf-8')).hexdigest()
    from ventanal.art.image_cache import prune
    if path.is_file():
        try:
            with Image.open(path) as image:
                image.load()
                result = image.copy()
            path.touch()  # Explicit LRU: filesystem atime may be disabled.
            prune(cache_dir)
            return result
        except (OSError, ValueError, SyntaxError):
            path.unlink(missing_ok=True)

    response = _get(client, url, attempts=attempts, rate_limit=rate_limit)
    content_type = (
        response.headers.get('content-type', '').split(';', 1)[0].strip().lower()
    )
    if not content_type.startswith('image/'):
        raise ValueError(f'Expected image response, got {content_type!r}')
    with Image.open(BytesIO(response.content)) as image:
        image.load()
        result = image.copy()

    cache_dir.mkdir(parents=True, exist_ok=True)
    temporary = None
    try:
        with NamedTemporaryFile(dir=cache_dir, delete=False) as stream:
            temporary = Path(stream.name)
            stream.write(response.content)
        os.replace(temporary, path)
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)
    prune(cache_dir)
    return result


def classify_kind(*fields):
    """Classify source type/technique metadata; never infer from artwork titles."""
    value = json.dumps(fields, ensure_ascii=False).casefold()
    groups = (
        (
            'manuscript/illumination',
            ('illuminat', 'manuscript', 'miniature', 'miniatur'),
        ),
        (
            'print',
            (
                'print',
                'etching',
                'engraving',
                'lithograph',
                'woodcut',
                'aquatint',
                'ætsning',
                'kobberstik',
                'radering',
                'houtsnede',
                'estampe',
                'gravure',
                'tryk',
            ),
        ),
        ('photograph', ('photograph', 'fotografi', 'gelatin silver', 'daguerreotype')),
        (
            'drawing',
            (
                'drawing',
                'drawings',
                'tekening',
                'tegning',
                'dessin',
                'chalk',
                'charcoal',
                'pencil',
            ),
        ),
        (
            'painting',
            (
                'painting',
                'oil on',
                'oil paint',
                'canvas',
                'maleri',
                'schilderij',
                'peinture',
                'watercolor',
                'watercolour',
                'aquarel',
                'tempera',
                'gouache',
            ),
        ),
    )
    return next(
        (kind for kind, terms in groups if any(t in value for t in terms)), 'other'
    )
