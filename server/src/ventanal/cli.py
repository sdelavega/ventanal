# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Stephen de la Vega

"""Command-line entry points for rendering, ticking and serving Ventanal."""

import argparse
from datetime import datetime
import json
import logging
from pathlib import Path
from zoneinfo import ZoneInfo

import httpx
from PIL import Image

from ventanal.config import load_config
from ventanal.art.sources import USER_AGENT
from ventanal import jobs, calibrate
from ventanal.art.render import ART_READOUT
from ventanal.frame import encode_frame
from ventanal.manifest import publish
from ventanal.quantize import quantize, simulate
from ventanal.security import Security, SecurityStateBusy, security_state_lock
from ventanal.serve import make_server


CERTIFICATE_COMMANDS = ('regenerate-certificate', 'restore-certificate')


def main(argv=None):
    parser = argparse.ArgumentParser(prog='ventanal')
    parser.add_argument('--config', type=Path, default=Path('/etc/ventanal/ventanal.toml'))
    commands = parser.add_subparsers(dest='command', required=True)
    for name in ('tick', 'serve', 'render-art', 'render-weather', 'chart', 'calibrate', 'skipped',
                 'setup-token', *CERTIFICATE_COMMANDS):
        command = commands.add_parser(name)
        command.add_argument('--config', type=Path, default=argparse.SUPPRESS)
        if name == 'skipped':
            command.add_argument('--limit', type=int, default=50)
        if name in CERTIFICATE_COMMANDS:
            command.add_argument('--confirm', action='store_true', required=True,
                                 help='Confirm the change to the certificate presented to the board')
        if name == 'tick':
            command.add_argument('--force-art', action='store_true',
                                 help='Republish the current artwork without advancing rotation')
            command.add_argument('--force-weather', action='store_true',
                                 help='Republish weather before its normal interval')
            command.add_argument('--force-spares', action='store_true',
                                 help='Re-render reserved spares without selecting new artwork')
        if name == 'calibrate':
            command.add_argument('photo', type=Path)
            command.add_argument('--corners', required=True, help='TL TR BR BL as x,y pairs')
            command.add_argument('--write', action='store_true')
            command.add_argument('--paper', help='White paper rectangle x,y,width,height in photo pixels')
        if name.startswith('render-'):
            command.add_argument('--out', type=Path, required=True)
    args = parser.parse_args(argv)
    logging.basicConfig(level=logging.INFO, format='%(levelname)s %(message)s')
    logging.getLogger('httpx').setLevel(logging.WARNING)
    cfg = load_config(args.config)
    if args.command == 'setup-token':
        from ventanal.auth import Auth
        print(Auth(cfg.cache_dir).issue_setup_token() or 'Setup is already complete.')
        return
    if args.command in ('serve', *CERTIFICATE_COMMANDS):
        try:
            with security_state_lock(cfg.cache_dir):
                if args.command == 'serve':
                    with make_server(cfg.output_dir, cfg.listen_host, cfg.listen_port,
                                     cache_dir=cfg.cache_dir, config_path=args.config) as server:
                        try:
                            server.serve_forever()
                        except KeyboardInterrupt:
                            pass
                    return
                security = Security(cfg.cache_dir)
                if args.command == 'restore-certificate':
                    if not security.restore():
                        parser.error(Security.NO_PREVIOUS_CERTIFICATE)
                    print(f'Restored certificate SHA-256: {security.fingerprint}')
                else:
                    if not security.ready:
                        parser.error('Enable security before regenerating its certificate.')
                    security.regenerate()
                    print(f'New certificate SHA-256: {security.fingerprint}')
        except SecurityStateBusy as exc:
            parser.error(str(exc))
        return
    now = datetime.now(ZoneInfo(cfg.timezone))
    if args.command == 'skipped':
        from ventanal.owner import OwnerStore
        if args.limit < 1:
            parser.error('--limit must be positive')
        for row in OwnerStore(cfg.cache_dir).skipped(args.limit):
            key = f"{row['source']}:{row['object_id']}"
            label = ' [ignored]' if key in cfg.ignored else ' [likely ignore]' if row['count'] >= 2 else ''
            print(f"{row['title']} — {row['artist']} | {row['museum']} | count={row['count']} | {row['last_skipped']}{label}")
            print(f'"{key}",  # {row["title"]} — {row["artist"]}')
        return
    if args.command == 'calibrate':
        try:
            corners = [tuple(map(int, pair.split(','))) for pair in args.corners.split()]
            with Image.open(args.photo) as photo:
                colours = calibrate.measure(photo, corners)
                if args.paper:
                    colours = calibrate.white_balance(colours, calibrate.paper_mean(photo, tuple(map(int, args.paper.split(',')))))
            if args.write:
                calibrate.write_panel(args.config, colours)
        except (OSError, ValueError) as exc:
            parser.error(str(exc))
        print(calibrate.panel_block(colours), end='')
        return
    if args.command == 'chart':
        indices = calibrate.chart()
        publish(cfg.output_dir, 'art', encode_frame(indices), ART_READOUT,
                {'title': 'Calibration chart', 'artist': 'Ventanal', 'date': '', 'museum': ''},
                simulate(indices, cfg.panel_rgb), now)
        return
    cfg.cache_dir.mkdir(parents=True, exist_ok=True)
    with httpx.Client(timeout=60, follow_redirects=True, headers={'User-Agent': USER_AGENT}) as client:
        if args.command == 'tick':
            print(json.dumps(jobs.tick(cfg, now, client, force_art=args.force_art, force_spares=args.force_spares, force_weather=args.force_weather)))
            return
        if args.command == 'render-art':
            pool = jobs.ArtPool(cfg.cache_dir / 'art.sqlite3')
            jobs.ensure_pool(cfg, pool, client, now)
            canvas, _, _ = jobs.art_canvas(cfg, now, client, pool)
        else:
            canvas = jobs.weather_canvas(cfg, now, client)
        args.out.parent.mkdir(parents=True, exist_ok=True)
        simulate(quantize(canvas, cfg.panel_rgb), cfg.panel_rgb).save(args.out, format='PNG')


if __name__ == '__main__':
    main()
