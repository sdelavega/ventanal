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

from ventanal.cache_access import cache_owner_error
from ventanal.config import ConfigError, load_config
from ventanal.art.sources import USER_AGENT
from ventanal import jobs, calibrate
from ventanal.art.render import ART_READOUT
from ventanal.frame import encode_frame
from ventanal.manifest import publish
from ventanal.quantize import quantize, simulate
from ventanal.security import PreviousCertificateHeld, Security, SecurityStateBusy, security_state_lock
from ventanal.serve import make_server


CERTIFICATE_COMMANDS = ('regenerate-certificate', 'restore-certificate')
CACHE_WRITERS = ('tick', 'serve', 'render-art', 'render-weather', 'skipped',
                 'setup-token', *CERTIFICATE_COMMANDS)


def _release_command(parser, args):
    import hashlib
    import os
    from cryptography.hazmat.primitives import serialization
    from cryptography.hazmat.primitives.asymmetric.ed25519 import (
        Ed25519PrivateKey, Ed25519PublicKey,
    )
    from ventanal import release

    def write_new(path, data, mode):
        fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, mode)
        try:
            with os.fdopen(fd, 'wb') as output:
                output.write(data)
        except OSError:
            path.unlink()
            raise

    try:
        if args.release_command == 'keygen':
            key = Ed25519PrivateKey.generate()
            write_new(args.private_key, key.private_bytes(
                serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8,
                serialization.NoEncryption(),
            ), 0o600)
            try:
                write_new(args.public_key, key.public_key().public_bytes(
                    serialization.Encoding.PEM, serialization.PublicFormat.SubjectPublicKeyInfo,
                ), 0o644)
            except OSError:
                args.private_key.unlink()
                raise
            return

        json_bytes = args.metadata.read_bytes()
        signature_path = args.signature or args.metadata.with_suffix(args.metadata.suffix + '.sig')
        if args.release_command == 'sign':
            key = serialization.load_pem_private_key(args.key.read_bytes(), password=None)
            if not isinstance(key, Ed25519PrivateKey):
                raise ValueError('Expected an Ed25519 private key')
            sig = release.sign_release(json_bytes, key)
            write_new(signature_path, sig.encode('ascii'), 0o644)
            return

        key = release.RELEASE_PUBLIC_KEY
        if args.key is not None:
            key = serialization.load_pem_public_key(args.key.read_bytes())
            if not isinstance(key, Ed25519PublicKey):
                raise ValueError('Expected an Ed25519 public key')
        sig = signature_path.read_bytes().decode('ascii')
        policy = (args.board, args.running, args.slot_size)
        if all(value is None for value in policy):
            info = release.verify_release(json_bytes, sig, key)
        elif any(value is None for value in policy):
            raise ValueError('--board, --running and --slot-size must be used together')
        else:
            info = release.check_release(json_bytes, sig, args.board, args.running, key, args.slot_size)
        if args.image is not None:
            image_hash = hashlib.sha256()
            size = 0
            with args.image.open('rb') as image:
                while chunk := image.read(65536):
                    image_hash.update(chunk)
                    size += len(chunk)
            if not info.matches_image(size, image_hash.digest()):
                raise ValueError('Image size or SHA-256 mismatch')
        print(f'Verified {info.board} {info.version}')
    except (OSError, ValueError, TypeError) as exc:
        parser.error(str(exc))


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
        if name == 'regenerate-certificate':
            command.add_argument('--discard-previous', '--force', action='store_true',
                                 help='Discard the saved rollback certificate before rotating again')
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
    release_parser = commands.add_parser('release', help='Manage firmware release signatures offline')
    release_commands = release_parser.add_subparsers(dest='release_command', required=True)
    keygen = release_commands.add_parser('keygen', help='Create an Ed25519 release keypair')
    keygen.add_argument('private_key', type=Path, help='New private PEM file to keep offline')
    keygen.add_argument('public_key', type=Path, help='New public PEM file')
    sign = release_commands.add_parser('sign', help='Sign canonical release metadata')
    verify = release_commands.add_parser('verify', help='Verify signed release metadata')
    for command in (sign, verify):
        command.add_argument('metadata', type=Path, help='Canonical release JSON file')
        command.add_argument('--signature', type=Path, help='Signature file (defaults to JSON path + .sig)')
    sign.add_argument('--key', type=Path, required=True, help='Ed25519 private PEM file')
    verify.add_argument('--key', type=Path, help='Ed25519 public PEM file (defaults to embedded key)')
    verify.add_argument('--image', type=Path, help='Firmware image to check against the signed digest')
    verify.add_argument('--board', choices=('inkplate13spectra', 'lcd43b'))
    verify.add_argument('--running', help='Currently installed firmware version')
    verify.add_argument('--slot-size', type=int, help='OTA slot size in bytes')
    args = parser.parse_args(argv)
    if args.command == 'release':
        return _release_command(release_parser, args)
    logging.basicConfig(level=logging.INFO, format='%(levelname)s %(message)s')
    logging.getLogger('httpx').setLevel(logging.WARNING)
    try:
        cfg = load_config(args.config)
    except ConfigError as exc:
        parser.error(str(exc))
    if args.command in CACHE_WRITERS:
        if error := cache_owner_error(cfg.cache_dir, f'ventanal {args.command} …'):
            parser.error(error)
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
                    security.regenerate(discard_previous=args.discard_previous)
                    print(f'New certificate SHA-256: {security.fingerprint}')
        except PreviousCertificateHeld as exc:
            parser.error(f'{exc} Use --discard-previous (or --force) to regenerate anyway.')
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
