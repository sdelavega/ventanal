# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Stephen de la Vega
from .base import BaseSource, classify_kind, Candidate, _get, _dimension
import httpx


class MetSource(BaseSource):
    id = name = 'met'

    def rights_filter(self, record):
        return record.get('isPublicDomain') is True

    def search_term(
        self, term: str, limit: int, client: httpx.Client
    ) -> list[Candidate]:
        if limit <= 0:
            return []
        # v1/search retired on 2026-10-01; v1.1 paginates the same objectIDs.
        data = _get(
            client,
            'https://collectionapi.metmuseum.org/public/collection/v1.1/search',
            params={'q': term, 'hasImages': 'true', 'offset': 0, 'limit': limit},
        ).json()
        candidates = []
        for ident in (data.get('objectIDs') or [])[:limit]:
            obj = _get(
                client,
                f'https://collectionapi.metmuseum.org/public/collection/v1/objects/{ident}',
            ).json()
            classification = (obj.get('classification') or '').lower()
            if not (
                self.rights_filter(obj)
                and obj.get('primaryImage')
                and any(
                    kind in classification for kind in ('painting', 'drawing', 'print')
                )
            ):
                continue
            candidates.append(
                Candidate(
                    source=self.id,
                    object_id=str(obj['objectID']),
                    title=obj.get('title') or '',
                    artist=obj.get('artistDisplayName') or '',
                    date=obj.get('objectDate') or '',
                    museum='The Metropolitan Museum of Art',
                    image_url=obj['primaryImage'],
                    width=None,
                    height=None,
                    kind=classify_kind(
                        obj.get('classification'),
                        obj.get('objectName'),
                        obj.get('medium'),
                    ),
                )
            )
        return candidates
