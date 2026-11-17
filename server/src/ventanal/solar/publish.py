# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Stephen de la Vega
"""Meaningful-change solar publication, serialized by the existing tick lock."""
from datetime import datetime
import hashlib
import json
import logging
from ventanal.manifest import read_manifest, write_manifest, publish
from ventanal.quantize import quantize, simulate
from ventanal.frame import encode_frame
from ventanal.solar.licence import verify
from ventanal.solar.data import fetch_solar, forecast_days
from ventanal.solar.render import render_solar, SOLAR_READOUT


def meaningful(s):
    return hashlib.sha256(json.dumps([s.sfi,s.ssn,s.daily_day,s.kp,forecast_days(s),s.sun_day],sort_keys=True).encode()).hexdigest()


def published_at(previous):
    try:
        return datetime.fromisoformat(previous.get('published', ''))
    except (AttributeError, TypeError, ValueError):
        return None


def due(previous,s,now):
    if not isinstance(previous,dict) or not previous: return True
    if previous.get('alerts') != s.alerts: return True
    published=published_at(previous)
    if published is None or published.utcoffset() is None: return True
    return previous.get('key') != meaningful(s) and (now-published).total_seconds()>=10800


def tick_solar(cfg,now,client):
    gate=verify(cfg.cache_dir/'fcc.sqlite3',cfg.ham_call_sign,cfg.zip,now)
    m=read_manifest(cfg.output_dir)
    if not gate.ok:
        if m['screens'].pop('solar',None) is not None: write_manifest(cfg.output_dir,m)
        return False
    try:
        s=fetch_solar(client,cfg.cache_dir,now)
        path=cfg.cache_dir/'solar-state.json'
        try: previous=json.loads(path.read_text())
        except (OSError,ValueError): previous={}
        if not isinstance(previous,dict): previous={}
        context=hashlib.sha256(repr((cfg.ham_call_sign,cfg.zip,cfg.latitude,cfg.longitude,cfg.panel_rgb)).encode()).hexdigest()
        # Configuration changes matter but retain the 3-hour bound except gate re-enable.
        if previous and previous.get('context')!=context: previous['key']=''
        published=published_at(previous)
        if published and published.utcoffset() is not None and previous.get('alerts') == s.alerts and (now-published).total_seconds()<10800:
            return False
        if 'solar' in m['screens'] and not due(previous,s,now): return False
        coords=(cfg.latitude,cfg.longitude)
        if None in coords:
            from ventanal.weather.data import geocode
            lat,lon,_=geocode(cfg,client,cfg.cache_dir)
            coords=(lat,lon)
        canvas=render_solar(s,cfg.ham_call_sign,coords,now,cfg)
        indices=quantize(canvas,cfg.panel_rgb)
        publish(cfg.output_dir,'solar',encode_frame(indices),SOLAR_READOUT,None,simulate(indices,cfg.panel_rgb),now)
        tmp=path.with_suffix('.tmp'); tmp.write_text(json.dumps({'key':meaningful(s),'alerts':s.alerts,'published':now.isoformat(),'context':context})); tmp.replace(path)
        return True
    except Exception:
        logging.getLogger(__name__).warning('Solar publication failed; previous frame retained')
        return False
