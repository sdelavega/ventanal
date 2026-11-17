# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Stephen de la Vega
"""Built-in source registry. New adapters register once here; UI uses declarations."""

import logging
import httpx
from .base import (
    Candidate,
    Credential,
    Source,
    MuseumSource,
    USER_AGENT,
    fetch_image,
    iiif_url,
)
from .met import MetSource
from .aic import AICSource
from .cleveland import ClevelandSource
from .rijksmuseum import RijksmuseumSource
from .nga import NGASource
from .paris import ParisSource
from .smk import SMKSource
from .commons import CommonsSource

logger = logging.getLogger('ventanal.art')


def failure_info(exc):
    # Retain local call frames, never exception text, chained bodies or request URLs.
    safe = RuntimeError('Source failure (' + type(exc).__name__ + ')')
    return (RuntimeError, safe, exc.__traceback__)


PLUGINS = (
    MetSource,
    AICSource,
    ClevelandSource,
    RijksmuseumSource,
    NGASource,
    ParisSource,
    SMKSource,
    CommonsSource,
)


def registry(*, cache_dir=None, credentials=None):
    sources = {}
    labels = {
        'met': 'The Met',
        'aic': 'Art Institute of Chicago',
        'cleveland': 'Cleveland Museum of Art',
    }
    for plugin in PLUGINS:
        source = plugin(
            cache_dir=cache_dir, credentials=(credentials or {}).get(plugin.id, {})
        )
        source.name = labels.get(source.id, source.name)
        sources[source.id] = source
    return sources


# Original import and patch seam retained for existing callers/tests.
SOURCES = {source.id: source() for source in (MetSource, AICSource, ClevelandSource)}


def configured_sources(cfg):
    sources = registry(cache_dir=cfg.cache_dir, credentials=cfg.credentials)
    # Existing integrations can continue supplying legacy source test doubles.
    for ident, source in SOURCES.items():
        sources[ident] = source
    return [sources[ident] for ident in cfg.museums if ident in sources]


def search_all(sources, terms, limit_per_term, client, *, unavailable=None):
    terms = tuple(terms)
    candidates = []
    unavailable = unavailable if unavailable is not None else set()
    for source in sources:
        ident = getattr(source, 'id', source.name)
        if ident in unavailable:
            continue
        for term in terms:
            try:
                if hasattr(source, 'id'):
                    candidates.extend(source.search(term, client, limit_per_term))
                else:
                    candidates.extend(source.search(term, limit_per_term, client))
            except httpx.HTTPStatusError as exc:
                if exc.response.status_code in (401, 403, 429):
                    unavailable.add(ident)
                    logger.warning(
                        'Museum source %s unavailable (HTTP %s); skipping remaining refresh terms',
                        ident,
                        exc.response.status_code,
                        exc_info=failure_info(exc),
                    )
                    break
                logger.warning(
                    'Museum source %s request failed', ident, exc_info=failure_info(exc)
                )
            except Exception as exc:
                logger.warning(
                    'Museum source %s request failed', ident, exc_info=failure_info(exc)
                )
    return candidates
