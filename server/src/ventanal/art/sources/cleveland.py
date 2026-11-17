# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Stephen de la Vega
from .base import BaseSource, classify_kind, Candidate, _get, _dimension
import httpx


class ClevelandSource(BaseSource):
    id = name = 'cleveland'

    def rights_filter(self, record):
        return record.get('share_license_status') == 'CC0'

    def search_term(
        self, term: str, limit: int, client: httpx.Client
    ) -> list[Candidate]:
        if limit <= 0:
            return []
        data = _get(
            client,
            'https://openaccess-api.clevelandart.org/api/artworks/',
            params={
                'q': term,
                'cc0': 1,
                'has_image': 1,
                'limit': limit,
            },
        ).json()
        candidates = []
        for obj in (data.get('data') or [])[:limit]:
            images = obj.get('images') or {}
            image = images.get('print') or {}
            if not image.get('url'):
                image = images.get('web') or {}
            if not image.get('url') or not self.rights_filter(obj):
                continue
            creators = obj.get('creators') or []
            candidates.append(
                Candidate(
                    source=self.id,
                    object_id=str(obj['id']),
                    title=obj.get('title') or '',
                    artist=(creators[0].get('description') or '') if creators else '',
                    date=obj.get('creation_date') or '',
                    museum='Cleveland Museum of Art',
                    image_url=image['url'],
                    width=_dimension(image.get('width')),
                    height=_dimension(image.get('height')),
                    kind=classify_kind(obj.get('type'), obj.get('technique')),
                )
            )
        return candidates
