# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Stephen de la Vega
"""Board telemetry and panel previews; all protocol additions are optional."""
from contextlib import closing
from datetime import datetime, timedelta, timezone
import hashlib
import json
import logging
import re
from zoneinfo import ZoneInfo
from ventanal.owner import OwnerStore


def parse_board(query):
    patterns = {'bat_mv': r'[1-9][0-9]{3}', 'bat': r'(?:0|[1-9][0-9]?|100)', 'screen': r'(?:art|weather|solar|extra-[a-z0-9_-]{1,42})',
                'frame': r'[0-9a-f]{16}', 'fw': r'[A-Za-z0-9][A-Za-z0-9._-]{0,63}'}
    result = {}
    for key, pattern in patterns.items():
        if key not in query: continue
        values = query[key]
        if len(values) != 1 or not re.fullmatch(pattern, values[0]):
            logging.getLogger(__name__).warning('board report malformed %s ignored', key)
            continue
        result[key] = int(values[0]) if key in ('bat', 'bat_mv') else values[0]
    return result


class BoardStore(OwnerStore):
    def __init__(self, directory):
        super().__init__(directory)
        with closing(self.connect()) as db, db:
            db.executescript('''CREATE TABLE IF NOT EXISTS board_checkins (
                time TEXT NOT NULL, ip TEXT NOT NULL, data TEXT NOT NULL);
                CREATE TABLE IF NOT EXISTS activity (time TEXT NOT NULL, kind TEXT NOT NULL, detail TEXT NOT NULL);
                CREATE TABLE IF NOT EXISTS frame_roles (frame_id TEXT PRIMARY KEY, role TEXT NOT NULL);''')

    def role(self, ident, role):
        with closing(self.connect()) as db, db:
            db.execute('INSERT OR IGNORE INTO frame_roles VALUES (?,?)', (ident, role))

    def frame_role(self, ident):
        with closing(self.connect()) as db:
            row = db.execute('SELECT role FROM frame_roles WHERE frame_id=?', (ident,)).fetchone()
        return row[0] if row else None

    def checkin(self, ip, values, *, timeout=120):
        with closing(self.connect(timeout=timeout)) as db, db:
            db.execute('INSERT INTO board_checkins VALUES (?,?,?)',
                       (datetime.now(timezone.utc).isoformat(), ip, json.dumps(values)))
            db.execute('DELETE FROM board_checkins WHERE rowid NOT IN (SELECT rowid FROM board_checkins ORDER BY rowid DESC LIMIT 500)')

    def latest(self):
        with closing(self.connect()) as db:
            row = db.execute('SELECT time, ip, data FROM board_checkins ORDER BY rowid DESC LIMIT 1').fetchone()
        return {'time': row[0], 'ip': row[1], **json.loads(row[2])} if row else None

    def event(self, kind, detail):
        with closing(self.connect()) as db, db:
            db.execute('INSERT INTO activity VALUES (?,?,?)', (datetime.now(timezone.utc).isoformat(), kind, detail))
            db.execute('DELETE FROM activity WHERE rowid NOT IN (SELECT rowid FROM activity ORDER BY rowid DESC LIMIT 500)')

    def recent(self):
        with closing(self.connect()) as db:
            rows = db.execute('''SELECT time, 'check-in', data FROM board_checkins
                UNION ALL SELECT time, kind, detail FROM activity
                UNION ALL SELECT time, 'skip', skipped || ' → ' || shown FROM skip_events
                ORDER BY time DESC LIMIT 1500''').fetchall()
        # Collapse consecutive equal reports within each event stream, including
        # alternating publish/check-in pairs. A changed report starts a new run.
        groups, latest = [], {}
        for t, k, d in rows:
            signature = json.dumps(json.loads(d), sort_keys=True) if k == 'check-in' else d
            previous = latest.get(k)
            if previous and previous['_signature'] == signature:
                previous['count'] += 1
                previous['first_time'] = t
            else:
                group = dict(time=t, first_time=t, kind=k, detail=d, count=1, _signature=signature)
                groups.append(group)
                latest[k] = group
        return [{k:v for k,v in group.items() if k != '_signature'} for group in groups[:12]]


def next_wake(cfg, now, low=False):
    """Mirror firmware's local-midnight boundaries, folds and quiet-end rule."""
    zone = ZoneInfo(cfg.timezone)
    local = now.astimezone(zone)
    interval = cfg.weather_interval_minutes * (4 if low else 1)
    candidates = []
    for day in range(2):
        date = local.date() + timedelta(days=day)
        for minute in range(0, 1440, interval):
            for fold in (0, 1):
                candidate = datetime.combine(date, datetime.min.time(), zone).replace(hour=minute//60, minute=minute%60, fold=fold)
                restored = datetime.fromtimestamp(candidate.timestamp(), zone)
                if restored.replace(tzinfo=None) != candidate.replace(tzinfo=None): continue
                if candidate.timestamp() > now.timestamp(): candidates.append(candidate)
    if not candidates: return datetime.fromtimestamp(now.timestamp()+60, zone)
    wake = min(candidates, key=lambda x:x.timestamp())
    start, end = [int(v[:2])*60+int(v[3:]) for v in (cfg.quiet_start,cfg.quiet_end)]
    minute = wake.hour*60 + wake.minute
    quiet = start <= minute < end if start < end else (minute >= start or minute < end) if start > end else False
    if quiet:
        date = wake.date() + timedelta(days=int(minute >= end))
        ends = [datetime.combine(date, datetime.min.time(), zone).replace(hour=end//60, minute=end%60, fold=f) for f in (0,1)]
        valid = [e for e in ends if e.timestamp() >= wake.timestamp() and datetime.fromtimestamp(e.timestamp(), zone).hour == end//60]
        wake = min(valid or ends[:1], key=lambda e:e.timestamp())
        wake = datetime.fromtimestamp(wake.timestamp(), zone)
    return datetime.fromtimestamp(max(wake.timestamp(),now.timestamp()+60), zone)


def showing(board, manifest, role=None):
    if not board: return 'Unknown'
    if board.get('screen','').startswith('extra-'): return board['screen'][6:].replace('-', ' ').title()
    if board.get('screen') == 'solar': return 'Solar'
    if board.get('screen') == 'weather': return 'Weather'
    ident = board.get('frame')
    if ident and ident == manifest.get('screens', {}).get('art', {}).get('id'): return 'Main art'
    for i, entry in enumerate(manifest.get('spares', [])):
        if ident == entry['id']: return f'Spare {i+1} of 3'
    if role in ('spare0','spare1','spare2'): return f'Spare {int(role[-1])+1} of 3 (previous frame)'
    if role == 'art': return 'Main art (previous frame)'
    return 'Art (previous frame)' if board.get('screen') == 'art' else 'Unknown'


def preview(cfg, entry):
    """Decode the published indices with the current configured panel palette."""
    from ventanal.frame import decode_frame
    from ventanal.quantize import simulate
    ident = entry['id']
    url = entry['url']
    if not re.fullmatch(r'[0-9a-f]{16}', ident) or not re.fullmatch(r'/frames/[A-Za-z0-9_-]+\.vnf', url):
        raise ValueError('invalid frame entry')
    key = ident + '-' + hashlib.sha256(repr(cfg.panel_rgb).encode()).hexdigest()[:16]
    target = cfg.cache_dir / 'ui-previews' / (key + '.png')
    if not target.exists():
        source = cfg.output_dir.resolve() / url.lstrip('/')
        if source.resolve() != source: raise ValueError('invalid frame path')
        image = simulate(decode_frame(source.read_bytes()), cfg.panel_rgb)
        target.parent.mkdir(parents=True, exist_ok=True)
        from io import BytesIO
        from ventanal.auth import private_write
        data = BytesIO(); image.save(data, format='PNG')
        private_write(target, data.getvalue())
    # Keep the newest generated files; tolerate concurrent preview pruning.
    files = []
    for path in target.parent.glob('*.png'):
        try: files.append((path.stat().st_mtime_ns, path))
        except FileNotFoundError: pass
    for _, old in sorted(files, reverse=True)[20:]:
        if old != target: old.unlink(missing_ok=True)
    return target
