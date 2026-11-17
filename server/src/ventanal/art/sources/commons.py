# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Stephen de la Vega
"""Opt-in Commons painting search; file-page rights, not Wikidata age guesses."""

from html import unescape
import re
from .base import BaseSource, classify_kind, Candidate, _get


def plain(value):
    return unescape(re.sub('<[^>]+>', '', value or '')).strip()


class CommonsSource(BaseSource):
    id = 'commons'
    name = 'Wikimedia Commons'
    description = 'Public-domain paintings, including the Louvre and Prado.'
    default_enabled = False
    rate_limit = 1
    legal_note = 'Photos of public-domain paintings are treated as public domain in the US (PD-Art); some countries differ.'

    def rights_filter(self, record):
        meta = record.get('extmetadata', {})
        licence = plain(meta.get('LicenseShortName', {}).get('value', '')).lower()
        usage = plain(meta.get('UsageTerms', {}).get('value', '')).lower()
        copyrighted = meta.get('Copyrighted', {}).get('value')
        categories = (
            plain(meta.get('Categories', {}).get('value', '')).lower().split('|')
        )
        painting = any('painting' in c for c in categories)
        cc0 = licence == 'cc0' and usage in (
            'cc0',
            'cc0 1.0 universal',
            'creative commons zero, public domain dedication',
        )
        pd = (
            licence in ('public domain', 'pd-art', 'pd-old')
            and usage == 'public domain'
            and copyrighted == 'False'
            and any(c.startswith(('pd-art', 'pd-old')) for c in categories)
        )
        return painting and (cc0 or pd)

    def search_term(self, term, limit, client):
        data = _get(
            client,
            'https://commons.wikimedia.org/w/api.php',
            rate_limit=self.rate_limit,
            params={
                'action': 'query',
                'format': 'json',
                'generator': 'search',
                'gsrnamespace': 6,
                'gsrsearch': term + ' filetype:bitmap painting',
                'gsrlimit': min(10, limit * 2),
                'prop': 'imageinfo',
                'iiprop': 'url|size|extmetadata',
                'iiurlwidth': 2400,
                'maxlag': 5,
            },
        ).json()
        if data.get('error'):
            raise ValueError('Commons API unavailable')
        result = []
        for page in data.get('query', {}).get('pages', {}).values():
            for image in page.get('imageinfo', [])[:1]:
                if not self.rights_filter(image):
                    continue
                meta = image.get('extmetadata', {})
                field = lambda key: plain(meta.get(key, {}).get('value', ''))
                # Artwork metadata (not a generic bitmap merely mentioning paintings).
                if not field('Artist') or not field('ObjectName'):
                    continue
                result.append(
                    Candidate(
                        self.id,
                        str(page['pageid']),
                        field('ObjectName'),
                        field('Artist'),
                        field('Date'),
                        field('Institution') or self.name,
                        image.get('thumburl') or image['url'],
                        image.get('thumbwidth', image.get('width')),
                        image.get('thumbheight', image.get('height')),
                        kind=classify_kind(
                            field('ObjectType'), field('Medium'), field('Categories')
                        ),
                    )
                )
                if len(result) >= limit:
                    return result
        return result
