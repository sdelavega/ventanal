# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Stephen de la Vega
"""Isolated settings work; share the timer's lock and never rotate art for filters."""
from datetime import datetime
import fcntl
import json
from pathlib import Path
import sys
from zoneinfo import ZoneInfo
import httpx
from ventanal.cache_access import cache_owner_error
from ventanal.config import load_config
from ventanal.art.pool import ArtPool, refresh_pool
from ventanal.jobs import _tick_locked
from ventanal.manifest import read_manifest, write_manifest


def main():
    cfg = load_config(Path(sys.argv[1]))
    command = 'python -m ventanal.settings_worker …'
    if error := cache_owner_error(cfg.cache_dir, command):
        raise SystemExit(error)
    flags = {'--refresh-pool':'pool', **{'--force-'+effect:effect for effect in ('art','weather','spares','schedule','solar')}}
    effects = {flags[arg] for arg in sys.argv[2:]}
    now = datetime.now(ZoneInfo(cfg.timezone))
    with (cfg.cache_dir / 'tick.lock').open('a') as lock, httpx.Client(timeout=30) as client:
        fcntl.flock(lock, fcntl.LOCK_EX)
        # A running tick may have held the lock while more settings arrived.
        cfg = load_config(Path(sys.argv[1]))
        if error := cache_owner_error(cfg.cache_dir, command):
            raise SystemExit(error)
        now = datetime.now(ZoneInfo(cfg.timezone))
        if 'schedule' in effects:
            manifest = read_manifest(cfg.output_dir)
            manifest['quiet'] = {'start':cfg.quiet_start, 'end':cfg.quiet_end}
            write_manifest(cfg.output_dir, manifest)
        if 'pool' in effects:
            refresh_pool(cfg, ArtPool(cfg.cache_dir / 'art.sqlite3'), client, now.date())
        if 'solar' in effects and not effects & {'art','weather','spares'}:
            from ventanal.solar.publish import tick_solar
            tick_solar(cfg, now, client)
        result = {}
        if effects & {'art','weather','spares'}:
            result = _tick_locked(cfg, now, client, 'art' in effects, 'spares' in effects, 'weather' in effects)
            if any(not result.get(key, True) for key in effects & {'art','weather','spares'}):
                raise RuntimeError('Settings render incomplete')
        print(json.dumps(result))


if __name__ == '__main__':
    main()
