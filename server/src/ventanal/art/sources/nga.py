# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Stephen de la Vega
"""Weekly compact local index joined from NGA's official bulk CSV exports."""

import csv
from io import StringIO
import json
import os
from tempfile import NamedTemporaryFile
import time
from .base import BaseSource, classify_kind, Candidate, _get, iiif_url


class NGASource(BaseSource):
    id = 'nga'
    name = 'National Gallery of Art'
    description = 'Washington open-access paintings from a weekly bulk index.'
    rate_limit = 1
    _index = None
    _loaded = 0
    refresh_seconds = 7 * 86400
    base_url = (
        'https://raw.githubusercontent.com/NationalGalleryOfArt/opendata/main/data/'
    )

    def rights_filter(self, record):
        return str(record.get('openaccess')) == '1'

    def index(self, client):
        if (
            self._index is not None
            and time.time() - self._loaded < self.refresh_seconds
        ):
            return self._index
        path = self.cache_dir / 'nga-index-v2.json' if self.cache_dir else None
        if (
            path
            and path.is_file()
            and time.time() - path.stat().st_mtime < self.refresh_seconds
        ):
            try:
                self._index = json.loads(path.read_text())
                self._loaded = path.stat().st_mtime
                return self._index
            except (OSError, ValueError):
                pass
        objects = {}
        for obj in csv.DictReader(
            StringIO(
                _get(
                    client, self.base_url + 'objects.csv', rate_limit=self.rate_limit
                ).text
            )
        ):
            if obj.get('classification', '').lower() not in (
                'painting',
                'drawing',
                'print',
            ):
                continue
            objects[obj['objectid']] = {
                k: obj.get(k, '')
                for k in (
                    'title',
                    'attribution',
                    'displaydate',
                    'classification',
                    'medium',
                )
            }
        rows = []
        for image in csv.DictReader(
            StringIO(
                _get(
                    client,
                    self.base_url + 'published_images.csv',
                    rate_limit=self.rate_limit,
                ).text
            )
        ):
            ident = image.get('depictstmsobjectid')
            if (
                ident not in objects
                or not self.rights_filter(image)
                or image.get('viewtype') != 'primary'
            ):
                continue
            if not image.get('iiifurl', '').startswith('https://api.nga.gov/iiif/'):
                continue
            obj = objects[ident]
            rows.append(
                dict(
                    source=self.id,
                    object_id=ident,
                    title=obj['title'],
                    artist=obj['attribution'],
                    date=obj['displaydate'],
                    museum=self.name,
                    image_url=iiif_url(image['iiifurl']),
                    width=int(image['width']) if image.get('width') else None,
                    height=int(image['height']) if image.get('height') else None,
                    kind=classify_kind(obj['classification'], obj['medium']),
                )
            )
        if not rows:
            raise ValueError('NGA index has no open images')
        if path:
            path.parent.mkdir(parents=True, exist_ok=True)
            with NamedTemporaryFile(dir=path.parent, mode='w', delete=False) as output:
                json.dump(rows, output)
                temporary = output.name
            os.replace(temporary, path)
        self._index = rows
        self._loaded = time.time()
        return rows

    def search_term(self, term, limit, client):
        # Bulk source has no remote keyword endpoint; match locally.
        return [
            Candidate(**r)
            for r in self.index(client)
            if term.casefold() in (r['title'] + ' ' + r['artist']).casefold()
        ][:limit]
