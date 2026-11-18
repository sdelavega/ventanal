# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Stephen de la Vega

"""Persistent seasonal artwork pool and no-repeat selection."""

from collections.abc import Iterable, Sequence
from contextlib import closing
from datetime import date, datetime, timedelta, timezone
from pathlib import Path
import random
import sqlite3

import httpx

from ventanal.art.sources import Candidate, search_all, configured_sources
from ventanal.config import Config, Season


def aspect_allowed(width: int | None, height: int | None) -> bool:
    """Allow up to 3:1 in either orientation, deferring unknown dimensions."""
    if not width or not height:
        return True
    return max(width, height) <= 3 * min(width, height)


def season_for(d: date, seasons: Sequence[Season]) -> Season:
    """Return the first matching inclusive month/day range, including wraps."""
    day = (d.month, d.day)
    for season in seasons:
        if season.includes(day):
            return season
    raise ValueError(f"No season matches {d.isoformat()}")


def _aware(value: datetime) -> datetime:
    if value.utcoffset() is None:
        raise ValueError("Expected a timezone-aware datetime")
    return value


class ArtPool:
    """Deduplicate artworks globally while preserving every season membership.

    Short-lived connections leave no open database handles between operations.
    ISO timestamps retain offsets; comparisons use aware UTC instants in Python.
    """

    def __init__(self, db_path: Path):
        self.db_path = db_path
        with closing(sqlite3.connect(self.db_path)) as db, db:
            db.executescript("""
                CREATE TABLE IF NOT EXISTS artworks (
                    source TEXT NOT NULL,
                    object_id TEXT NOT NULL,
                    title TEXT NOT NULL,
                    artist TEXT NOT NULL,
                    date TEXT NOT NULL,
                    museum TEXT NOT NULL,
                    image_url TEXT NOT NULL,
                    width INTEGER,
                    height INTEGER,
                    last_shown TEXT,
                    PRIMARY KEY (source, object_id)
                );
                CREATE TABLE IF NOT EXISTS artwork_seasons (
                    source TEXT NOT NULL,
                    object_id TEXT NOT NULL,
                    season TEXT NOT NULL,
                    PRIMARY KEY (source, object_id, season)
                );
                CREATE TABLE IF NOT EXISTS pool_metadata (
                    key TEXT PRIMARY KEY,
                    value TEXT NOT NULL
                );
            """)
            columns = {row[1] for row in db.execute("PRAGMA table_info(artworks)")}
            if "kind" not in columns:
                db.execute(
                    "ALTER TABLE artworks ADD COLUMN kind TEXT NOT NULL DEFAULT 'other'"
                )
            if "colour_fraction" not in columns:
                db.execute("ALTER TABLE artworks ADD COLUMN colour_fraction REAL")
            for name, kind in [
                ("fit_score", "REAL"),
                ("fit_palette", "TEXT"),
                ("fit_attempt", "REAL"),
                ("fit_failures", "INTEGER NOT NULL DEFAULT 0"),
                ("fit_retry_after", "REAL"),
            ]:
                if name not in columns:
                    db.execute(f"ALTER TABLE artworks ADD COLUMN {name} {kind}")
            policy = "oklab-chroma-.02-fraction-.08-outside60-.03-metadata-v2"
            previous = db.execute(
                "SELECT value FROM pool_metadata WHERE key = 'colour_policy'"
            ).fetchone()
            if previous is None or previous[0] != policy:
                db.execute("UPDATE artworks SET colour_fraction = NULL")
                db.execute(
                    "INSERT OR REPLACE INTO pool_metadata VALUES ('colour_policy', ?)",
                    (policy,),
                )

    def add(self, candidates: Iterable[Candidate], season: str) -> int:
        """Return the number of new artworks, excluding new season memberships."""
        added = 0
        with closing(sqlite3.connect(self.db_path)) as db, db:
            for c in candidates:
                result = db.execute(
                    """
                    INSERT INTO artworks
                        (source, object_id, title, artist, date, museum, image_url, width, height, kind)
                    VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
                    ON CONFLICT (source, object_id) DO NOTHING
                """,
                    (
                        c.source,
                        c.object_id,
                        c.title,
                        c.artist,
                        c.date,
                        c.museum,
                        c.image_url,
                        c.width,
                        c.height,
                        c.kind,
                    ),
                )
                added += result.rowcount
                # Refresh download metadata without resetting display history.
                db.execute(
                    """
                    UPDATE artworks SET
                        fit_failures = CASE WHEN image_url = ? THEN fit_failures ELSE 0 END,
                        fit_retry_after = CASE WHEN image_url = ? THEN fit_retry_after ELSE NULL END,
                        fit_palette = CASE WHEN image_url = ? THEN fit_palette ELSE NULL END,
                        colour_fraction = CASE WHEN image_url = ? AND kind = ? THEN colour_fraction ELSE NULL END,
                        image_url = ?, width = ?, height = ?, kind = ?
                    WHERE source = ? AND object_id = ?
                """,
                    (
                        c.image_url,
                        c.image_url,
                        c.image_url,
                        c.image_url,
                        c.kind,
                        c.image_url,
                        c.width,
                        c.height,
                        c.kind,
                        c.source,
                        c.object_id,
                    ),
                )
                db.execute(
                    """
                    INSERT INTO artwork_seasons (source, object_id, season)
                    VALUES (?, ?, ?) ON CONFLICT DO NOTHING
                """,
                    (c.source, c.object_id, season),
                )
        return added

    def pick_next(
        self,
        season: str,
        now: datetime,
        no_repeat_days: int,
        rng: random.Random,
        *,
        excluded=(),
        museums=(),
        strict=False,
        include_monotone=False,
        screen_fit_only=False,
        panel_rgb=(),
    ) -> Candidate | None:
        """Prefer suitable seasonal images, then other seasons, then oldest shown.

        The final fallback includes small eligible images. Never-shown images
        precede shown images in that fallback.
        Picking alone does not mark an artwork as shown.
        """
        cutoff = _aware(now).astimezone(timezone.utc) - timedelta(days=no_repeat_days)
        with closing(sqlite3.connect(self.db_path)) as db:
            rows = db.execute(
                """
                SELECT a.source, a.object_id, a.title, a.artist, a.date, a.museum,
                       a.image_url, a.width, a.height, a.last_shown,
                       EXISTS (SELECT 1 FROM artwork_seasons s
                               WHERE s.source = a.source AND s.object_id = a.object_id
                                 AND s.season = ?), a.kind, a.colour_fraction, a.fit_score, a.fit_palette
                FROM artworks a ORDER BY a.source, a.object_id
            """,
                (season,),
            ).fetchall()
        if not rows:
            return None

        from ventanal.art.screen_fit import fingerprint, THRESHOLD

        key = fingerprint(panel_rgb)
        scores = {}
        entries = []
        eligible = []
        seasonal = []
        for row in rows:
            c = Candidate(*row[:9], kind=row[11])
            if not aspect_allowed(c.width, c.height):
                continue
            if (
                not include_monotone
                and c.kind not in ("painting", "manuscript/illumination")
                and row[12] is not None
                and row[12] < 0.08
            ):
                continue
            if f"{c.source}:{c.object_id}" in excluded or (
                museums and c.source not in museums
            ):
                continue
            shown = (
                _aware(datetime.fromisoformat(row[9])).astimezone(timezone.utc)
                if row[9]
                else None
            )
            scores[(c.source, c.object_id)] = row[13] if row[14] == key else None
            entries.append((c, shown))
            size_ok = (
                c.width is None or c.height is None or max(c.width, c.height) >= 1600
            )
            if size_ok and (shown is None or shown <= cutoff):
                eligible.append(c)
                if row[10]:
                    seasonal.append(c)

        choices = seasonal if strict else seasonal or eligible
        if screen_fit_only:

            def fit(c):
                return scores[(c.source, c.object_id)]

            import logging

            log = logging.getLogger(__name__)
            if strict and not choices:
                log.warning(
                    "Screen-fit strict selection empty; considering available pool entries"
                )
            available = choices or [c for c, _ in entries]
            passing = [
                c for c in available if fit(c) is not None and fit(c) >= THRESHOLD
            ]
            if passing:
                choices = passing
            elif available:
                known = [c for c in available if fit(c) is not None]
                if known:
                    log.warning("Screen-fit fallback: best available artwork")
                    choices = [max(known, key=fit)]
                else:
                    log.warning("Screen-fit unscored provisional selection")
                    choices = available
        if choices:
            weights = [
                (
                    3
                    if c.width is not None
                    and c.height is not None
                    and c.width >= c.height
                    else 1
                )
                for c in choices
            ]
            return rng.choices(choices, weights=weights, k=1)[0]
        if strict or not entries:
            return None
        earliest = datetime.min.replace(tzinfo=timezone.utc)
        return min(entries, key=lambda entry: entry[1] or earliest)[0]

    def get(self, source: str, object_id: str) -> Candidate | None:
        """Look up a published artwork without selecting or updating history."""
        with closing(sqlite3.connect(self.db_path)) as db:
            row = db.execute(
                """
                SELECT source, object_id, title, artist, date, museum,
                       image_url, width, height, kind FROM artworks
                WHERE source = ? AND object_id = ?
            """,
                (source, object_id),
            ).fetchone()
        return Candidate(*row) if row else None

    def colour_status(self, candidate: Candidate) -> bool | None:
        if candidate.kind in ("painting", "manuscript/illumination"):
            return True
        with closing(sqlite3.connect(self.db_path)) as db:
            row = db.execute(
                "SELECT colour_fraction FROM artworks WHERE source = ? AND object_id = ? AND image_url = ?",
                (candidate.source, candidate.object_id, candidate.image_url),
            ).fetchone()
        return None if not row or row[0] is None else row[0] >= 0.08

    def assess(self, candidate: Candidate, image) -> bool:
        """Persist a decoded-image assessment without touching artwork/history."""
        from ventanal.art.colour import colour_fraction

        fraction = (
            1.0
            if candidate.kind in ("painting", "manuscript/illumination")
            else colour_fraction(image)
        )
        with closing(sqlite3.connect(self.db_path)) as db, db:
            db.execute(
                "UPDATE artworks SET colour_fraction = ? WHERE source = ? AND object_id = ? AND image_url = ?",
                (fraction, candidate.source, candidate.object_id, candidate.image_url),
            )
        return fraction >= 0.08

    def fit_score(self, candidate, palette):
        from ventanal.art.screen_fit import fingerprint

        with closing(sqlite3.connect(self.db_path)) as db:
            row = db.execute(
                "SELECT fit_score FROM artworks WHERE source=? AND object_id=? AND image_url=? AND fit_palette=?",
                (
                    candidate.source,
                    candidate.object_id,
                    candidate.image_url,
                    fingerprint(palette),
                ),
            ).fetchone()
        return row[0] if row else None

    def assess_fit(self, candidate, image, palette):
        from ventanal.art.screen_fit import assess, fingerprint

        result = assess(image, palette)
        with closing(sqlite3.connect(self.db_path)) as db, db:
            db.execute(
                "UPDATE artworks SET fit_score=?, fit_palette=?, fit_failures=0, fit_retry_after=NULL WHERE source=? AND object_id=? AND image_url=?",
                (
                    result.score,
                    fingerprint(palette),
                    candidate.source,
                    candidate.object_id,
                    candidate.image_url,
                ),
            )
        return result.score

    def mark_shown(self, c: Candidate, now: datetime) -> None:
        timestamp = _aware(now).isoformat()
        with closing(sqlite3.connect(self.db_path)) as db, db:
            db.execute(
                "UPDATE artworks SET last_shown = ? WHERE source = ? AND object_id = ?",
                (timestamp, c.source, c.object_id),
            )

    def last_refresh(self) -> datetime | None:
        with closing(sqlite3.connect(self.db_path)) as db:
            row = db.execute(
                "SELECT value FROM pool_metadata WHERE key = 'last_refresh'"
            ).fetchone()
        return _aware(datetime.fromisoformat(row[0])) if row else None

    def set_refreshed(self, now: datetime) -> None:
        timestamp = _aware(now).isoformat()
        with closing(sqlite3.connect(self.db_path)) as db, db:
            db.execute(
                """
                INSERT INTO pool_metadata (key, value) VALUES ('last_refresh', ?)
                ON CONFLICT (key) DO UPDATE SET value = excluded.value
            """,
                (timestamp,),
            )


def refresh_pool(cfg: Config, pool: ArtPool, client: httpx.Client, today: date, *, now: datetime | None = None) -> int:
    """Search this season and the first season starting after its end date."""
    current = season_for(today, cfg.seasons)
    upcoming = min(cfg.seasons, key=lambda s: (s.start <= current.end, s.start))
    sources = configured_sources(cfg)
    added = 0
    found_candidates = False
    unavailable = set()
    for season in (current,) if upcoming == current else (current, upcoming):
        candidates = search_all(
            sources,
            season.terms,
            limit_per_term=40,
            client=client,
            unavailable=unavailable,
        )
        if candidates:
            found_candidates = True
            added += pool.add(candidates, season.name)
    if found_candidates:
        pool.set_refreshed(now if now is not None else datetime.now(timezone.utc))
    return added
