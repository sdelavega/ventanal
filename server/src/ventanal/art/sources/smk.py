# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Stephen de la Vega
"""SMK's public-domain search with IIIF images."""

from .base import BaseSource, classify_kind, Candidate, _get, iiif_url, public_rights


class SMKSource(BaseSource):
    id = 'smk'
    name = 'SMK — National Gallery of Denmark'
    description = 'Danish and European public-domain art.'
    rate_limit = 2

    def rights_filter(self, record):
        return record.get('public_domain') is True and public_rights(
            record.get('rights')
        )

    def search_term(self, term, limit, client):
        data = _get(
            client,
            'https://api.smk.dk/api/v1/art/search/',
            rate_limit=self.rate_limit,
            params={
                'keys': term,
                'rows': min(100, limit * 3),
                'filters': '[public_domain:true],[has_image:true]',
            },
        ).json()
        result = []
        for obj in data.get('items', []):
            if (
                not self.rights_filter(obj)
                or not obj.get('has_image')
                or not obj.get('image_iiif_id')
            ):
                continue
            result.append(
                Candidate(
                    self.id,
                    obj['id'].removesuffix('_object'),
                    next(
                        (t['title'] for t in obj.get('titles', []) if t.get('title')),
                        '',
                    ),
                    ', '.join(obj.get('artist', [])),
                    next(
                        (d.get('period', '') for d in obj.get('production_date', [])),
                        '',
                    ),
                    self.name,
                    iiif_url(obj['image_iiif_id']),
                    obj.get('image_width'),
                    obj.get('image_height'),
                    kind=classify_kind(obj.get('object_names'), obj.get('techniques')),
                )
            )
            if len(result) >= limit:
                break
        return result
