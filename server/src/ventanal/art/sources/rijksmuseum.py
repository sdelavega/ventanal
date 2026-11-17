# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Stephen de la Vega
"""Keyless Linked Art search and visual-item image rights."""

from .base import BaseSource, classify_kind, Candidate, iiif_url, public_rights


def text(records, kind=None):
    return next(
        (r.get('content', '') for r in records if not kind or r.get('type') == kind), ''
    )


class RijksmuseumSource(BaseSource):
    id = 'rijksmuseum'
    name = 'Rijksmuseum'
    description = 'Dutch paintings through keyless Linked Art and IIIF.'
    rate_limit = 4

    def resolve(self, uri, client):
        ident = uri.rsplit('/', 1)[-1]
        if not ident.isdecimal():
            raise ValueError('Invalid Rijksmuseum identifier')
        return self.cached_json(
            ident,
            client,
            f'https://data.rijksmuseum.nl/{ident}',
            params={'_profile': 'la-framed'},
        )

    def rights_filter(self, record):
        # The visual item's rights, never the metadata's subject_of CC0 licence.
        rights = record.get('subject_to', [])
        return bool(rights) and all(
            any(public_rights(c.get('id')) for c in r.get('classified_as', []))
            for r in rights
        )

    def search_term(self, term, limit, client):
        data = self.cached_json(
            'search:' + term,
            client,
            'https://data.rijksmuseum.nl/search/collection',
            params={'title': term, 'type': 'painting', 'imageAvailable': 'true'},
        )
        result = []
        for item in data.get('orderedItems', [])[: min(100, limit)]:
            obj = self.resolve(item['id'], client)
            production = obj.get('produced_by', {})
            for shown in obj.get('shows', []):
                visual = self.resolve(shown['id'], client)
                if not self.rights_filter(visual):
                    continue
                for digital in visual.get('digitally_shown_by', [])[:1]:
                    image = self.resolve(digital['id'], client)
                    access = image.get('access_point', [])
                    if not access or '/full/' not in access[0]['id']:
                        continue
                    service = access[0]['id'].split('/full/')[0]
                    info = self.cached_json(service, client, service + '/info.json')
                    result.append(
                        Candidate(
                            self.id,
                            item['id'].rsplit('/', 1)[-1],
                            text(obj.get('identified_by', []), 'Name'),
                            text(production.get('referred_to_by', [])),
                            text(
                                production.get('timespan', {}).get('identified_by', [])
                            ),
                            self.name,
                            iiif_url(service),
                            info.get('width'),
                            info.get('height'),
                            kind=classify_kind(
                                obj.get('classified_as'), obj.get('made_of')
                            ),
                        )
                    )
                    break
                if len(result) >= limit:
                    return result
        return result
