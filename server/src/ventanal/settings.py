# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Stephen de la Vega
"""Comment-preserving settings edits using the loader's validation."""
from datetime import datetime, timezone
import fcntl
import os
from pathlib import Path
import secrets
import re
import time
import hashlib
import json
import tomlkit
from ventanal.config import validate_config
from ventanal.art.sources import registry

# Managed values remain visible here; dedicated pages own their edits.
FIELDS = {
    'ham.call_sign':'text',
    'location.zip':'text', 'location.latitude':'float', 'location.longitude':'float',
    'location.place_name':'text', 'location.timezone':'text',
    'display.units':('imperial','metric'), 'display.clock':('12h','24h'),
    'schedule.quiet_start':'text', 'schedule.quiet_end':'text',
    'schedule.weather_interval_minutes':'int', 'schedule.art_slots':'list',
    'art.include_monotone':'bool',
    'art.screen_fit_only':'bool',
    'art.museums':'museums', 'art.rijksmuseum_key':'secret', 'art.no_repeat_days':'int', 'art.ignored':'list',
    'weather.radar':('iem','none'), 'weather.radar_radius_miles':'int',
    'weather.here_label':('off','en','es'), 'weather.nws_contact':'text',
    **{'panel.'+c:'text' for c in ('black','white','yellow','red','blue','green')},
    'server.listen':'text', 'server.https_listen':'text', 'server.output_dir':'text', 'server.cache_dir':'text',
}
SOURCE_REGISTRY = registry()
MUSEUMS = tuple(SOURCE_REGISTRY)
MUSEUM_LABELS = {key:source.name for key,source in SOURCE_REGISTRY.items()}
CREDENTIAL_FIELDS = {f'art.credentials.{source.id}.{c.name}': c.label for source in SOURCE_REGISTRY.values() for c in source.credentials}
FIELDS.update({key:'secret' for key in CREDENTIAL_FIELDS})


TIME_ERROR = 'Enter a valid time, for example 22:00 or 10 pm.'


def parse_time(value):
    """Accept human clock notation and return the loader's canonical HH:MM."""
    raw = re.sub(r'\s+', '', str(value).lower()).replace('a.m.', 'am').replace('p.m.', 'pm')
    match = re.fullmatch(r'(\d{1,2})(?:[:.](\d{2}))?(am|pm)?', raw)
    if match:
        hour, minute, suffix = match.groups()
        hour, minute = int(hour), int(minute or 0)
        if suffix:
            if not 1 <= hour <= 12: raise ValueError(TIME_ERROR)
            hour = hour % 12 + (12 if suffix == 'pm' else 0)
    elif re.fullmatch(r'\d{3,4}', raw):
        hour, minute = int(raw[:-2]), int(raw[-2:])
    else: raise ValueError(TIME_ERROR)
    if hour > 23 or minute > 59: raise ValueError(TIME_ERROR)
    return f'{hour:02}:{minute:02}'


def read_document(path):
    with path.open() as source:
        fcntl.flock(source, fcntl.LOCK_SH)
        return source.read()


def edit_document(doc, values):
    values = dict(values)
    for key in list(values):
        if key.endswith('.month') and key.startswith('art.seasons['):
            base = key.removesuffix('.month')
            values[base] = values[key] + '-' + values.get(base+'.day', '')
    for key, kind in FIELDS.items():
        if key not in values or key in READ_ONLY: continue
        if key in CREDENTIAL_FIELDS:
            values[key] = values[key].strip()
            if not values[key] and values.get(key+'.clear') != 'yes': continue
            _, _, ident, name = key.split('.')
            doc['art'].setdefault('credentials', tomlkit.table()).setdefault(ident, tomlkit.table())[name] = '' if values.get(key+'.clear') == 'yes' else values[key]
            continue
        section, name = key.split('.')
        value = values[key]
        if isinstance(value, str): value = value.strip()
        if isinstance(kind, tuple): value = value.lower()
        if key == 'location.zip' and re.fullmatch(r'\d{9}', value): value = value[:5] + '-' + value[5:]
        try:
            if key in ('schedule.quiet_start', 'schedule.quiet_end'): value = parse_time(value)
            if key == 'schedule.art_slots':
                value = ','.join(sorted({parse_time(v) for v in value.split(',') if v.strip()}))
                if not value: raise ValueError('At least one art change time is required.')
        except ValueError as exc: raise ValueError(f'{key}: {exc}') from None
        if kind == 'secret' and values.get(key+'.clear') == 'yes': value = ''
        elif kind == 'secret' and not value: continue  # blank retains the stored key
        if key.startswith('location.') and name in ('zip','latitude','longitude','place_name') and value == '':
            doc[section].pop(name, None); continue
        try:
            if kind == 'bool':
                value = value == 'yes'
            elif kind == 'int':
                value = int(value)
            elif kind == 'float': value = float(value)
            elif kind == 'list': value = [v.strip() for v in value.split(',') if v.strip()]
            elif kind == 'museums':
                value = value if isinstance(value, list) else [value]
                if any(v not in MUSEUMS for v in value): raise ValueError()
        except (ValueError, TypeError): raise ValueError(f'{key}: invalid value') from None
        if key == 'ham.call_sign':
            from ventanal.solar.licence import normalize_call
            value = normalize_call(value)
        doc.setdefault(section, tomlkit.table())[name] = value
    for key, value in values.items():
        match = re.fullmatch(r'art.seasons\[(\d+)\]\.(name|start|end|terms)', key)
        if match and 'seasons.count' not in values:
            index, name = int(match[1]), match[2]
            if index >= len(doc['art']['seasons']): raise ValueError('art.seasons: unknown row')
            value = value.strip()
            doc['art']['seasons'][index][name] = [v.strip() for v in value.split(',') if v.strip()] if name == 'terms' else value
    if 'seasons.count' in values:
        count = season_count(values['seasons.count'])
        seasons = doc['art']['seasons']
        # Update existing tables in place to retain their comments and order.
        while len(seasons) > count: seasons.pop()
        while len(seasons) < count: seasons.append(tomlkit.table())
        for i in range(count):
            for name in ('name','start','end','terms'):
                value = values.get(f'art.seasons[{i}].{name}', '')
                seasons[i][name] = [v.strip() for v in value.split(',') if v.strip()] if name == 'terms' else value.strip()
    validate_config(doc.unwrap())
    return doc


def save_settings(path: Path, values, backup_dir=None, *, mutate=None, browser_session=None, now=None):
    backup_dir = backup_dir or path.parent / 'backups'
    # All browser saves serialize; tick consumes each file atomically.
    backup_dir.mkdir(parents=True, exist_ok=True)
    with (backup_dir / '.settings.lock').open('a') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        original = read_document(path)
        doc = edit_document(tomlkit.parse(original), values)
        if mutate is not None:
            mutate(doc)
            validate_config(doc.unwrap())
        backup_dir.mkdir(parents=True, exist_ok=True)
        stamp = datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%S.%fZ')
        from ventanal.auth import private_write
        clock = time.time() if now is None else now
        metadata = backup_dir / '.browser-backups.json'
        try: sessions = json.loads(metadata.read_text())
        except (OSError, ValueError): sessions = {}
        ident = hashlib.sha256(browser_session.encode()).hexdigest() if browser_session else None
        if ident is None or ident not in sessions or clock - sessions[ident] >= 3600:
            private_write(backup_dir / f'ventanal-{stamp}.toml', original.encode())
            if ident:
                sessions = {k:v for k,v in sessions.items() if clock-v < 30*86400}
                sessions[ident] = clock
                private_write(metadata, json.dumps(sessions).encode())
        # Preserve root/group ownership and mode on the existing config inode;
        # service cannot chown a newly replaced file to root. Lock readers too.
        with path.open('r+') as output:
            fcntl.flock(output, fcntl.LOCK_EX)
            output.write(tomlkit.dumps(doc)); output.truncate(); output.flush(); os.fsync(output.fileno())
        for old in sorted(backup_dir.glob('ventanal-*.toml'))[:-10]: old.unlink()
    return validate_config(doc.unwrap())


READ_ONLY = {'art.ignored', *('panel.'+c for c in ('black','white','yellow','red','blue','green'))}
ADVANCED = {'location.latitude', 'location.longitude', 'location.timezone', 'server.listen', 'server.https_listen', 'server.output_dir', 'server.cache_dir'}
FIELD_HELP = {
    'ham.call_sign': ('Call sign', 'US amateur licence only. Checked locally against the FCC database; registration ZIP must match your location ZIP.'),
    'location.zip': ('ZIP code', 'US ZIP or ZIP+4.'),
    'location.latitude': ('Latitude override', 'Optional decimal degrees; supply longitude too.'),
    'location.longitude': ('Longitude override', 'Optional decimal degrees; supply latitude too.'),
    'location.place_name': ('Place name', 'Optional name shown above the weather map.'),
    'location.timezone': ('Timezone', 'IANA timezone used for schedules, for example America/New_York.'),
    'display.units': ('Units', 'Temperature and weather measurement units.'),
    'display.clock': ('Clock', 'Time format shown on the display.'),
    'schedule.quiet_start': ('Sleep hours — start', 'Scheduled updates pause during sleep.'),
    'schedule.quiet_end': ('Sleep hours — end', 'Scheduled updates resume at this time.'),
    'schedule.weather_interval_minutes': ('Weather update interval (minutes)', 'Minutes between scheduled weather updates.'),
    'schedule.art_slots': ('Art change times', 'Choose one or more daily art change times.'),
    'art.screen_fit_only': ('Only show art that suits this screen', 'Prefer artwork whose colours and tones reproduce well with your calibrated panel.'),
    'art.include_monotone': (
        'Include monotone works (black-and-white or single-colour prints, drawings and photographs)',
        'Allow black-and-white and single-hue works; colour works are selected by default.',
    ),
    'art.museums': ('Museums', 'Choose the museum collections used for artwork.'),
    'art.rijksmuseum_key': ('Rijksmuseum API key', 'Optional; leave blank to keep the stored key.'),
    'art.no_repeat_days': ("Don't repeat art for (days)", 'Minimum number of days before an artwork may return.'),
    'art.ignored': ('Ignored artworks', 'Managed on the Art page; read-only here.'),
    'weather.radar': ('Weather radar', 'Show precipitation radar or turn it off.'),
    'weather.radar_radius_miles': ('Radar radius (miles)', 'Distance from your location to the edge of the radar map.'),
    'weather.here_label': ('Map caption', 'Caption marking your location on the map.'),
    'weather.nws_contact': ('NWS contact email', 'Sent only to the US National Weather Service; optional.'),
    'server.https_listen': ('HTTPS listen address', 'TLS host and port; defaults to 0.0.0.0:8766. Requires a service restart.'),
    'server.listen': ('Server listen address', 'Host and port; requires a service restart.'),
    'server.output_dir': ('Output directory', 'Published frame directory; requires a service restart and write access.'),
    'server.cache_dir': ('Cache directory', 'Cached data directory; requires a service restart and write access.'),
    **{'panel.'+c: (c.title()+' ink', 'Set by Calibration; read-only here.') for c in ('black','white','yellow','red','blue','green')},
}
FIELD_HELP.update({key:(label, 'Leave blank to keep the stored credential.') for key,label in CREDENTIAL_FIELDS.items()})
OPTION_LABELS = {
    'display.units': {'imperial': 'Imperial °F', 'metric': 'Metric °C'},
    'display.clock': {'12h': '12-hour', '24h': '24-hour'},
    'weather.here_label': {'off': 'Off', 'en': 'English “YOU ARE HERE”', 'es': 'Spanish “ESTÁS AQUÍ”'},
    'weather.radar': {'iem': 'Precipitation radar (IEM)', 'none': 'Off'},
}


def season_count(value):
    try:
        count = int(value)
        if not 1 <= count <= 50: raise ValueError()
    except (ValueError, TypeError):
        raise ValueError('art.seasons: expected 1–50 rows') from None
    return count
