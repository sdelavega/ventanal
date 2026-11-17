# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Stephen de la Vega
"""Paris Musées public GraphQL; token and live schema verification required.

The upstream explorer was unavailable during implementation. Introspect before
using provisional fields; refuse schema drift rather than dropping rights checks.
"""

import json
from .base import BaseSource, classify_kind, Candidate, Credential, request


class ParisSource(BaseSource):
    id = 'paris'
    name = 'Paris Musées'
    description = 'City of Paris collections; requires a free API token.'
    credentials = (Credential('token', 'Paris Musées API token'),)
    default_enabled = False
    rate_limit = (
        1 / 2
    )  # Public quota is 1000 requests/day; refresh only, never per tick.

    def rights_filter(self, record):
        return record.get('fieldOeuvreLibre') is True

    def search_term(self, term, limit, client):
        token = self._credentials.get('token')
        if not token:
            return []
        schema = request(
            client,
            'POST',
            'https://apicollections.parismusees.paris.fr/graphql',
            rate_limit=self.rate_limit,
            headers={'auth-token': token},
            json={
                'query': '{ oeuvre: __type(name: "NodeOeuvre") { fields { name } } media: __type(name: "MediaImage") { fields { name } } }'
            },
        ).json()
        types = schema.get('data') or {}
        for name, required in (
            ('oeuvre', {'nid', 'title', 'fieldOeuvreLibre', 'fieldVisuelsPrincipals'}),
            ('media', {'fieldMediaImage'}),
        ):
            fields = {f['name'] for f in (types.get(name) or {}).get('fields', [])}
            if not required <= fields:
                raise ValueError('Paris Musées source schema requires verification')
        # Public API exposes HD files only for open content; also require the
        # explicit oeuvre-libre flag rather than admitting public thumbnails.
        query = '''{ nodeQuery(limit: %d, filter: {conditions: [
          {field: "type", value: ["oeuvre"]},
          {field: "title", value: [%s], operator: LIKE}
        ]}) { entities { ... on NodeOeuvre {
          nid title fieldOeuvreLibre
          fieldVisuelsPrincipals { entity { ... on MediaImage { fieldMediaImage { url width height } } } }
        } } } }''' % (min(100, limit * 3), json.dumps('%' + term + '%'))
        data = request(
            client,
            'POST',
            'https://apicollections.parismusees.paris.fr/graphql',
            rate_limit=self.rate_limit,
            headers={'auth-token': token},
            json={'query': query},
        ).json()
        if data.get('errors'):
            raise ValueError('Paris Musées query unavailable')
        result = []
        for obj in data.get('data', {}).get('nodeQuery', {}).get('entities', []):
            if not self.rights_filter(obj):
                continue
            for visual in obj.get('fieldVisuelsPrincipals', []):
                image = (visual.get('entity') or {}).get('fieldMediaImage') or {}
                if (
                    not image.get('url')
                    or max(image.get('width', 0), image.get('height', 0)) < 1600
                ):
                    continue
                result.append(
                    Candidate(
                        self.id,
                        str(obj['nid']),
                        obj.get('title', ''),
                        '',
                        '',
                        self.name,
                        image['url'],
                        image.get('width'),
                        image.get('height'),
                        kind=classify_kind(),
                    )
                )
                break
            if len(result) >= limit:
                break
        return result
