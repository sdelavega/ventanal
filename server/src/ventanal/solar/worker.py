# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Stephen de la Vega
"""Background FCC refresh; no credentials or upstream errors in output."""
from datetime import datetime, timezone
import fcntl
from pathlib import Path
import sys
import httpx
from ventanal.cache_access import cache_owner_error
from ventanal.config import load_config
from ventanal.solar.licence import refresh, RefreshRunning
from ventanal.solar.publish import tick_solar


def main():
    cfg=load_config(Path(sys.argv[1])); now=datetime.now(timezone.utc)
    command = 'python -m ventanal.solar.worker …'
    if error := cache_owner_error(cfg.cache_dir, command):
        print(error, file=sys.stderr)
        return 1
    try:
        with httpx.Client(timeout=45) as client:
            if not refresh(cfg.cache_dir,client,now,force=True): return 1
            with (cfg.cache_dir/'tick.lock').open('a') as lock:
                fcntl.flock(lock,fcntl.LOCK_EX)
                cfg=load_config(Path(sys.argv[1]))
                if error := cache_owner_error(cfg.cache_dir, command):
                    print(error, file=sys.stderr)
                    return 1
                tick_solar(cfg,now,client)
    except RefreshRunning:
        print('Database refresh already running')
        return 2
    except Exception as exc:
        print(f'FCC worker failed: {type(exc).__name__} errno={getattr(exc,"errno",None)} sqlite={getattr(exc,"sqlite_errorname",None)}',file=sys.stderr)
        return 1
    return 0

if __name__=='__main__': sys.exit(main())
