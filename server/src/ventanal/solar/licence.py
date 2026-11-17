# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Stephen de la Vega
"""Local FCC ULS gate. US licences only; never transmit a configured call sign.

Official weekly complete amateur licence archive, verified 2026-11-16.
ULS pipe records: HD call/status/expiry = 5/6/9, EN licensee ZIP = 19,
AM operator class = 6 (one-based). Join by unique system identifier (2).
Only call/status/expiry/class/ZIP5 survive the temporary staging database.
"""
from dataclasses import dataclass
from datetime import datetime, timezone
import fcntl
import os
from pathlib import Path
import re
import sqlite3
import tempfile
from zipfile import ZipFile
from ventanal.weather.data import USER_AGENT

class RefreshRunning(Exception):
    """Another process owns the FCC import lock."""


FCC_URL = 'https://data.fcc.gov/download/pub/uls/complete/l_amat.zip'
WEEK = 7 * 86400
CLASSES = {'E':'Extra', 'A':'Advanced', 'G':'General', 'T':'Technician', 'N':'Novice', 'P':'Technician Plus'}


def normalize_call(value):
    value = value.strip().upper()
    if value and not re.fullmatch(r'[AKNW][A-Z]{0,2}[0-9][A-Z]{1,3}', value):
        raise ValueError('Enter a US amateur call sign, for example W1AW.')
    return value


@dataclass(frozen=True)
class Verification:
    ok: bool
    reason: str
    refreshed: str | None = None


def verify(db, call, zip_code, now):
    if not Path(db).exists():
        return Verification(False, 'Licence database not yet downloaded')
    try:
        with sqlite3.connect(f'file:{db}?mode=ro', uri=True) as c:
            refreshed = c.execute('select refreshed from metadata').fetchone()[0]
            row = c.execute('select status,expiry,class,zip from licences where call=?', (call,)).fetchone()
        if not call: return Verification(False, 'Enter your US amateur call sign', refreshed)
        if not row: return Verification(False, 'Call sign not found', refreshed)
        status, expiry, grade, registered_zip = row
        if status != 'A': return Verification(False, 'Licence inactive', refreshed)
        if not expiry or expiry < now.date().isoformat(): return Verification(False, 'Licence expired', refreshed)
        if not zip_code or registered_zip != zip_code[:5]: return Verification(False, 'ZIP mismatch', refreshed)
        return Verification(True, f'Verified — active {CLASSES.get(grade,"amateur")} licence, ZIP matches', refreshed)
    except (sqlite3.Error, TypeError, IndexError):
        return Verification(False, 'Licence database unreadable; refresh the licence database')


def build_index(archive, db, now):
    """Disk-backed joins, fixed SQLite cache, one bounded line at a time; atomic swap."""
    db = Path(db); db.parent.mkdir(parents=True, exist_ok=True)
    fd, name = tempfile.mkstemp(prefix='fcc-index-', suffix='.sqlite3', dir=db.parent)
    os.close(fd)
    try:
        with sqlite3.connect(name) as c, ZipFile(archive) as z:
            c.executescript('pragma cache_size=-4096; pragma temp_store=FILE; create table hd(id text primary key,call text,status text,expiry text); create table en(id text primary key,zip text); create table am(id text primary key,class text);')
            for kind, sql in [('HD','insert or replace into hd values(?,?,?,?)'),('EN','insert or replace into en values(?,?)'),('AM','insert or replace into am values(?,?)')]:
                count = 0
                with z.open(kind+'.dat') as source:
                    while True:
                        line = source.readline(65537)
                        if not line: break
                        if len(line)>65536: raise ValueError('ULS record too long')
                        f = line.decode('latin-1').rstrip('\r\n').split('|')
                        if f[0] != kind: continue
                        if kind == 'HD':
                            try: expiry = datetime.strptime(f[8], '%m/%d/%Y').date().isoformat()
                            except ValueError: expiry = ''
                            values = (f[1],f[4],f[5],expiry)
                        elif kind == 'EN':
                            if f[5] != 'L': continue
                            values = (f[1], f[18][:5])
                        else: values = (f[1],f[5])
                        c.execute(sql, values); count += 1
                        if count % 10000 == 0: c.commit()
                if not count: raise ValueError('Empty ULS table')
            c.executescript('create table licences(call text primary key,status text,expiry text,class text,zip text); insert or replace into licences select call,status,expiry,class,zip from hd join en using(id) left join am using(id) order by expiry; drop table hd; drop table en; drop table am; create table metadata(refreshed text);')
            c.execute('insert into metadata values(?)',(now.isoformat(),)); c.commit()
            c.execute('vacuum')
        os.replace(name, db)
    finally:
        Path(name).unlink(missing_ok=True)


def refresh(state_dir, client, now, force=False):
    """Cross-process single-flight; raw archive is removed on every exit."""
    state_dir = Path(state_dir); state_dir.mkdir(parents=True, exist_ok=True)
    with (state_dir/'fcc.lock').open('a') as lock:
        try: fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError: raise RefreshRunning from None
        db = state_dir/'fcc.sqlite3'
        v = verify(db, '', None, now)
        if not force and v.refreshed and (now-datetime.fromisoformat(v.refreshed)).total_seconds()<WEEK: return False
        raw = state_dir/'fcc-download.zip'
        try:
            with client.stream('GET', FCC_URL, timeout=300, follow_redirects=True, headers={'User-Agent': USER_AGENT}) as response:
                response.raise_for_status()
                size=0
                with raw.open('wb') as out:
                    for chunk in response.iter_bytes(65536):
                        size += len(chunk)
                        if size > 512*1024*1024: raise ValueError('ULS archive too large')
                        out.write(chunk)
            build_index(raw, db, now)
            return True
        finally: raw.unlink(missing_ok=True)
