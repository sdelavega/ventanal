# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Stephen de la Vega
from .base import BaseSource, classify_kind, Candidate, _get, _dimension
import httpx


class AICSource(BaseSource):
    id = name = 'aic'

    def rights_filter(self, record):
        # Require the returned rights flag as well as the server-side query filter.
        return record.get('is_public_domain') is True

    def search_term(
        self, term: str, limit: int, client: httpx.Client
    ) -> list[Candidate]:
        if limit <= 0:
            return []
        data = _get(
            client,
            'https://api.artic.edu/api/v1/artworks/search',
            params={
                'q': term,
                'query[term][is_public_domain]': 'true',
                'limit': limit,
                'fields': 'id,title,artist_title,date_display,image_id,thumbnail,classification_title,is_public_domain,artwork_type_title,medium_display',
            },
        ).json()
        candidates = []
        for obj in (data.get('data') or [])[:limit]:
            if not obj.get('image_id') or not self.rights_filter(obj):
                continue
            thumbnail = obj.get('thumbnail') or {}
            original_width = _dimension(thumbnail.get('width'))
            original_height = _dimension(thumbnail.get('height'))
            # 1686 is the largest permitted full-image width for public domain art.
            width = min(1686, original_width) if original_width else None
            height = (
                round(original_height * width / original_width)
                if original_height and original_width
                else None
            )
            candidates.append(
                Candidate(
                    source=self.id,
                    object_id=str(obj['id']),
                    title=obj.get('title') or '',
                    artist=obj.get('artist_title') or '',
                    date=obj.get('date_display') or '',
                    museum='Art Institute of Chicago',
                    # IIIF refuses to upscale (403 "scales in excess of 100%"), so
                    # never request wider than the original.
                    image_url=f"https://www.artic.edu/iiif/2/{obj['image_id']}/full/{width or 1686},/0/default.jpg",
                    width=width,
                    height=height,
                    kind=classify_kind(
                        obj.get('classification_title'),
                        obj.get('artwork_type_title'),
                        obj.get('medium_display'),
                    ),
                )
            )
        return candidates
