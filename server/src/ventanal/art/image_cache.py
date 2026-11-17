# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Stephen de la Vega
"""Shared size-bounded museum image LRU, including legacy root entries.

Referenced main/spare images are pinned even if they alone exceed the cap.
Only hash-named image files are managed; metadata and other caches are untouched.
"""
from contextlib import closing
from hashlib import sha256
import fcntl
import json
import os
from pathlib import Path
import re
import sqlite3
from tempfile import NamedTemporaryFile

from PIL import Image

DEFAULT_BYTES = 2 * 1024**3


def root_for(directory):
    directory = Path(directory)
    return directory.parent.parent if directory.parts[-2:] == ('sources', 'images') else directory


def configure(cfg):
    """Persist nonsecret policy so short-lived screen-fit workers share it."""
    root = cfg.cache_dir
    root.mkdir(parents=True, exist_ok=True)
    policy = json.dumps({'max_bytes': cfg.image_cache_bytes, 'output_dir': str(cfg.output_dir)})
    path = root / 'image-cache-policy.json'
    if path.exists() and path.read_text() == policy:
        return
    with NamedTemporaryFile(dir=root, mode='w', delete=False) as stream:
        stream.write(policy)
        temporary = Path(stream.name)
    os.replace(temporary, path)


def prune(directory, *, max_bytes=None):
    root = root_for(directory)
    root.mkdir(parents=True, exist_ok=True)
    with (root / 'image-cache.lock').open('a') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        _prune(root, max_bytes=max_bytes)


def _prune(root, *, max_bytes=None):
    policy = {}
    try:
        policy = json.loads((root / 'image-cache-policy.json').read_text())
    except (OSError, ValueError):
        pass
    if not isinstance(policy, dict):
        policy = {}
    limit = max_bytes if max_bytes is not None else policy.get('max_bytes', DEFAULT_BYTES)
    protected = set()
    db_path = root / 'art.sqlite3'
    if db_path.exists():
        # Resolve the manifest's main ID and every durable spare, including slots
        # not yet advertised. On broken metadata, fail closed against eviction.
        try:
            main = None
            if policy.get('output_dir'):
                from ventanal.manifest import read_manifest
                main = read_manifest(Path(policy['output_dir'])).get('screens', {}).get('art', {}).get('id')
            with closing(sqlite3.connect(db_path)) as db:
                ids = [json.loads(row[0])['id'] for row in db.execute('SELECT entry FROM spare_slots')]
                if main:
                    ids.append(main)
                for ident in ids:
                    row = db.execute('SELECT metadata FROM published_art WHERE frame_id=?', (ident,)).fetchone()
                    if not row:
                        # Legacy/incomplete identity ledger: retain images until
                        # the normal publisher backfills the current reference.
                        return
                    if row:
                        protected.add(sha256(json.loads(row[0])['image_url'].encode()).hexdigest())
        except (sqlite3.Error, ValueError, KeyError, TypeError):
            return
    # Remember validated content across worker processes. A changed size/mtime
    # invalidates the proof, so truncated entries are decoded and removed too.
    validation_path = root / 'image-cache-valid.json'
    try:
        validated = json.loads(validation_path.read_text())
        if not isinstance(validated, dict):
            validated = {}
    except (OSError, ValueError):
        validated = {}
    proofs = {}
    entries = []
    for directory in (root, root / 'sources' / 'images'):
        if not directory.exists():
            continue
        for path in directory.iterdir():
            if path.is_symlink() or not path.is_file() or not re.fullmatch('[0-9a-f]{64}', path.name):
                continue
            try:
                info = path.stat()
            except FileNotFoundError:
                continue
            key = str(path.relative_to(root))
            proof = [info.st_size, info.st_mtime_ns]
            if path.name not in protected and validated.get(key) != proof:
                try:
                    with Image.open(path) as image:
                        image.load()
                except (OSError, ValueError, SyntaxError, Image.DecompressionBombError):
                    path.unlink(missing_ok=True)
                    continue
            if path.name not in protected or validated.get(key) == proof:
                proofs[key] = proof
            entries.append((info.st_mtime_ns, path, info.st_size))
    total = sum(size for _, _, size in entries)
    for _, path, size in sorted(entries):
        if total <= limit:
            break
        if path.name not in protected:
            path.unlink(missing_ok=True)
            total -= size
            proofs.pop(str(path.relative_to(root)), None)
    temporary = validation_path.with_suffix('.tmp')
    temporary.write_text(json.dumps(proofs))
    os.replace(temporary, validation_path)
