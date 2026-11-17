# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Stephen de la Vega
"""Durable artwork identities, spare slots and idempotent owner feedback."""
from contextlib import closing
from dataclasses import asdict
from datetime import timezone
import json
import logging
import re
import sqlite3

log = logging.getLogger(__name__)
SPARES = 3


def parse_skips(value: str) -> list[tuple[str, str]]:
    pairs = value.split(',')
    if len(pairs) > 8:
        log.warning('skip report limit exceeded; ignoring excess pairs')
    result = []
    for pair in pairs[:8]:
        if re.fullmatch(r'[0-9a-fA-F]{16}~[0-9a-fA-F]{16}', pair):
            result.append(tuple(pair.lower().split('~')))
        else:
            log.warning('skip report malformed pair ignored')
    return result


class OwnerStore:
    def __init__(self, cache_dir):
        cache_dir.mkdir(parents=True, exist_ok=True)
        from ventanal.art.pool import ArtPool
        ArtPool(cache_dir / 'art.sqlite3')
        self.path = cache_dir / 'art.sqlite3'
        with closing(self.connect()) as db, db:
            db.executescript('''
                CREATE TABLE IF NOT EXISTS published_art (
                    frame_id TEXT PRIMARY KEY, metadata TEXT NOT NULL);
                CREATE TABLE IF NOT EXISTS spare_slots (
                    slot INTEGER PRIMARY KEY, entry TEXT NOT NULL,
                    source TEXT NOT NULL, object_id TEXT NOT NULL, replace_due INTEGER NOT NULL DEFAULT 0);
                CREATE TABLE IF NOT EXISTS ignore_replacements (slot INTEGER PRIMARY KEY);
                CREATE TABLE IF NOT EXISTS skip_events (
                    skipped TEXT NOT NULL, shown TEXT NOT NULL, time TEXT NOT NULL,
                    metadata TEXT NOT NULL, PRIMARY KEY(skipped, shown));
            ''')

    def connect(self):
        return sqlite3.connect(self.path, timeout=120)

    def register(self, ident, candidate):
        with closing(self.connect()) as db, db:
            db.execute('INSERT OR IGNORE INTO published_art VALUES (?,?)',
                       (ident, json.dumps(asdict(candidate))))

    def spares(self):
        with closing(self.connect()) as db:
            return [(slot, json.loads(entry), source, object_id, bool(due))
                    for slot, entry, source, object_id, due in db.execute(
                        'SELECT slot, entry, source, object_id, replace_due FROM spare_slots ORDER BY slot')]

    def put_spare(self, slot, entry, candidate, *, replace_due=False):
        self.register(entry['id'], candidate)
        with closing(self.connect()) as db, db:
            # Preserve an ignore-only cause across palette re-renders of this identity.
            old = db.execute('SELECT source, object_id FROM spare_slots WHERE slot=?', (slot,)).fetchone()
            if old != (candidate.source, candidate.object_id) or not replace_due:
                db.execute('DELETE FROM ignore_replacements WHERE slot=?', (slot,))
            db.execute('INSERT OR REPLACE INTO spare_slots VALUES (?,?,?,?,?)',
                       (slot, json.dumps(entry), candidate.source, candidate.object_id, int(replace_due)))

    def record(self, skipped, shown, now):
        """One transaction records feedback, shown history and replacement flag.

        Callers take tick.lock too, so publication and feedback cannot race.
        A permanent pair key permits safe NVS retries across service restarts.
        """
        with closing(self.connect()) as db, db:
            db.execute('BEGIN IMMEDIATE')
            old = db.execute('SELECT metadata FROM published_art WHERE frame_id=?', (skipped,)).fetchone()
            new = db.execute('SELECT metadata FROM published_art WHERE frame_id=?', (shown,)).fetchone()
            if not old or not new:
                log.warning('skip report unknown frame ignored')
                return False
            inserted = db.execute('INSERT OR IGNORE INTO skip_events VALUES (?,?,?,?)',
                                  (skipped, shown, now.astimezone(timezone.utc).isoformat(), old[0])).rowcount
            if not inserted:
                return False
            candidate = json.loads(new[0])
            db.execute('UPDATE artworks SET last_shown=? WHERE source=? AND object_id=?',
                       (now.isoformat(), candidate['source'], candidate['object_id']))
            for slot, entry in db.execute('SELECT slot, entry FROM spare_slots').fetchall():
                if json.loads(entry)['id'] == shown:
                    # A board skip is an independent replacement reason.
                    db.execute('DELETE FROM ignore_replacements WHERE slot=?', (slot,))
                    db.execute('UPDATE spare_slots SET replace_due=1 WHERE slot=?', (slot,))
        log.info('skip recorded skipped=%s shown=%s', skipped, shown)
        return True

    def skipped(self, limit=50):
        with closing(self.connect()) as db:
            rows = db.execute('SELECT metadata, time FROM skip_events ORDER BY time DESC').fetchall()
        grouped = {}
        for metadata, at in rows:
            c = json.loads(metadata)
            key = (c['source'], c['object_id'])
            if key not in grouped:
                grouped[key] = {**c, 'count': 0, 'last_skipped': at}
            grouped[key]['count'] += 1
        return sorted(grouped.values(), key=lambda c: (c['count'], c['last_skipped']), reverse=True)[:limit]
