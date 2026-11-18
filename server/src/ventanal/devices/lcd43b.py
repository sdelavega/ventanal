# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Stephen de la Vega

"""Full-colour 800x480 profile for the Waveshare ESP32-S3-Touch-LCD-4.3B.

The board renders raw RGB565 frames over an 800x480 parallel LCD; no
six-colour quantization applies. Frames use the VNR1 format: a 16-byte
header (magic, little-endian width and height, eight zero bytes) followed
by little-endian RGB565 pixels. The per-device manifest mirrors the main
manifest's schema (id/url/size) and adds the full SHA-256 so the firmware
can hash-verify every download.
"""

from dataclasses import replace

import hashlib
import json
import logging
import os
import struct

from pathlib import Path

import numpy as np
from ventanal.devices.lcd_draw import LCDDraw, rounded_mask
from PIL import Image, ImageDraw, ImageFilter

from ventanal.art.compose import compose
from ventanal.manifest import read_manifest, write_manifest
from ventanal.devices.lcd_weather import weather_frame
from ventanal.text import fit, font, renderable

LCD_W, LCD_H = 800, 480
FRAME_SIZE = 16 + LCD_W * LCD_H * 2  # VNR1 header + RGB565 pixels: 768016
_HEADER = struct.pack('<4sHH8x', b'VNR1', LCD_W, LCD_H)

DEVICE_URL = '/devices/lcd43b'
SCREENS = ('art', 'weather', 'solar')
SPARES = 3
RETAIN = 4


def device_url(cfg) -> str:
    """Publish URL prefix for the configured device id."""
    return f'/devices/{cfg.device_id}'


def device_dir(cfg) -> Path:
    """Output directory that mirrors the device's URL path."""
    return cfg.output_dir / 'devices' / cfg.device_id

log = logging.getLogger(__name__)


def encode_frame(image: Image.Image) -> bytes:
    """Encode an RGB 800x480 image as a little-endian RGB565 VNR1 frame."""
    if image.mode != 'RGB' or image.size != (LCD_W, LCD_H):
        raise ValueError('frame source must be an RGB 800x480 image')
    pixels = np.asarray(image, dtype=np.uint8)
    value = ((pixels[..., 0].astype(np.uint16) >> 3) << 11
             | (pixels[..., 1].astype(np.uint16) >> 2) << 5
             | (pixels[..., 2].astype(np.uint16) >> 3))
    return _HEADER + value.astype('<u2').tobytes()


def decode_frame(data: bytes) -> np.ndarray:
    """Validate a VNR1 frame and return its 480x800x3 RGB888 pixels."""
    if len(data) != FRAME_SIZE or data[:16] != _HEADER:
        raise ValueError('invalid VNR1 frame size or header')
    packed = np.frombuffer(data, dtype='<u2', offset=16).reshape(LCD_H, LCD_W)
    red = ((packed >> 11) & 0x1F).astype(np.uint8) << 3
    green = ((packed >> 5) & 0x3F).astype(np.uint8) << 2
    blue = (packed & 0x1F).astype(np.uint8) << 3
    return np.stack((red, green, blue), axis=-1)


def render_art(image: Image.Image, credit) -> Image.Image:
    """Compose native 800x480 art with the soft-shade credit; the board draws time."""
    backdrop = compose(image, size=(LCD_W, LCD_H))
    title_font, secondary_font = font('inter-bold', 12), font('inter', 11)
    title = renderable(credit.get('title') or '', 'inter-bold') or 'Untitled'
    fields = [renderable(credit.get(key) or '', 'inter')
              for key in ('artist', 'date', 'museum')]
    available = 772
    title = fit(title, title_font, available * .6)
    remaining = available - int(title_font.getlength(title))
    # Preserve the artist, dropping museum and then date before fitting.
    while True:
        secondary = ' · '.join(value for value in fields if value)
        secondary = '   ' + secondary if secondary else ''
        if secondary_font.getlength(secondary) <= remaining or len(fields) == 1:
            break
        fields.pop()
    secondary = fit(secondary, secondary_font, remaining)
    width = title_font.getlength(title) + secondary_font.getlength(secondary)
    mask = Image.new('L', backdrop.size)
    ImageDraw.Draw(mask).rounded_rectangle((0, 458, 14 + width + 20, 480),
                                           15, fill=150)
    backdrop.paste(Image.new('RGB', backdrop.size), (0, 0),
                   mask.filter(ImageFilter.GaussianBlur(7)))
    draw = LCDDraw(backdrop)
    draw.text((14, 474), title, font=title_font, fill='white', anchor='ls')
    draw.text((14 + title_font.getlength(title), 474), secondary,
              font=secondary_font, fill='white', anchor='ls')
    return backdrop


def _solar_allowed(cfg, now) -> bool:
    if not cfg.ham_call_sign:
        return False
    from ventanal.solar.licence import verify
    try:
        return verify(cfg.cache_dir / 'fcc.sqlite3', cfg.ham_call_sign,
                      cfg.zip, now).ok
    except Exception as exc:
        log.warning('lcd43b solar licence check failed: %s', exc)
        return False


def _art_image(cfg, client, credit):
    """Fetch the artwork behind a published credit from the shared pool."""
    source, object_id = credit.get('source'), credit.get('object_id')
    if not source or not object_id:
        raise ValueError('published credit lacks source/object_id')
    from ventanal import jobs
    pool = jobs.ArtPool(cfg.cache_dir / 'art.sqlite3')
    candidate = pool.get(source, object_id)
    if candidate is None:
        raise ValueError(f'artwork {source}:{object_id} absent from the pool')
    return jobs.fetch_candidate(candidate, client, cfg)


def _render_screen(cfg, name, now, client) -> Image.Image:
    if name == 'art':
        credit = (read_manifest(cfg.output_dir)['screens']
                  .get('art', {}).get('credit') or {})
        return render_art(_art_image(cfg, client, credit), credit)
    if name == 'weather':
        return weather_frame(cfg, now, client)
    if name == 'solar':
        from ventanal.solar.data import fetch_solar
        from ventanal.devices.lcd_solar import render_lcd_solar
        from ventanal.weather.data import geocode
        coords = (cfg.latitude, cfg.longitude)
        if None in coords:
            lat, lon, _ = geocode(cfg, client, cfg.cache_dir)
            coords = (lat, lon)
        return render_lcd_solar(fetch_solar(client, cfg.cache_dir, now),
                                replace(cfg, latitude=coords[0], longitude=coords[1]), now)
    raise ValueError(f'unknown screen: {name}')


def _publish_frame(out, base_url: str, name: str, data: bytes, now) -> dict:
    """Atomically publish an encoded frame and return its manifest entry."""
    digest = hashlib.sha256(data).hexdigest()
    ident = digest[:16]
    frames = out / 'frames'
    frames.mkdir(parents=True, exist_ok=True)
    path = frames / f'{name}-{ident}.vnr'
    temporary = frames / f'.{name}-{ident}.tmp'
    temporary.write_bytes(data)
    os.replace(temporary, path)
    # Prune old frames, always preserving the frame just published.
    superseded = sorted(
        (old for old in frames.glob(f'{name}-*.vnr') if old != path),
        key=lambda old: old.stat().st_mtime_ns,
        reverse=True,
    )
    for old in superseded[RETAIN - 1:]:
        old.unlink(missing_ok=True)
    return {
        'id': ident,
        'url': f'{base_url}/frames/{name}-{ident}.vnr',
        'size': len(data),
        'sha256': digest,
        'rendered': now.isoformat(timespec='seconds'),
    }


def _read_device_manifest(out) -> dict:
    try:
        manifest = json.loads((out / 'manifest.json').read_text(encoding='utf-8'))
        if not isinstance(manifest, dict):
            raise ValueError('expected a JSON object')
        return manifest
    except (OSError, ValueError):
        return {}


def tick(cfg, now, client) -> dict:
    """Render and publish the lcd43b profile's screens; failures are per-screen.

    The caller holds the tick lock, so the device manifest cannot race the
    e-paper manifest. A failed screen keeps the previous device entry.
    """
    if now.utcoffset() is None:
        raise ValueError('now must be timezone-aware')
    out = device_dir(cfg)
    base_url = device_url(cfg)
    previous = _read_device_manifest(out)
    screens = previous.get('screens') if isinstance(previous.get('screens'), dict) else {}
    screens = dict(screens)
    # Stale entries published under a previous device id are unreachable.
    screens = {name: entry for name, entry in screens.items()
               if entry.get('url', '').startswith(f'{base_url}/frames/')}
    for name in ('art', 'weather'):
        try:
            screens[name] = _publish_frame(out, base_url, name,
                                           encode_frame(_render_screen(cfg, name, now, client)),
                                           now)
        except Exception as exc:
            log.warning('lcd43b %s render failed; previous frame retained: %s',
                        name, exc)
    if _solar_allowed(cfg, now):
        try:
            screens['solar'] = _publish_frame(
                out, base_url, 'solar',
                encode_frame(_render_screen(cfg, 'solar', now, client)), now)
        except Exception as exc:
            log.warning('lcd43b solar render failed; previous frame retained: %s', exc)
    else:
        screens.pop('solar', None)
    spares = _render_spares(cfg, now, client, out, base_url)
    manifest = {
        'version': 1,
        'clock_24h': cfg.clock_24h,
        'screens': {name: screens[name] for name in SCREENS if name in screens},
        'quiet': {'start': cfg.quiet_start, 'end': cfg.quiet_end},
    }
    if spares:
        manifest['spares'] = spares
    write_manifest(out, manifest)
    return manifest


def _render_spares(cfg, now, client, out, base_url) -> list:
    """Mirror the owner's published spare slots at 800x480, best effort."""
    from ventanal import jobs
    entries = read_manifest(cfg.output_dir).get('spares')
    if not isinstance(entries, list):
        return []
    spares = []
    previous = _read_device_manifest(out).get('spares')
    previous = previous if isinstance(previous, list) else []
    for index, entry in enumerate(entries[:SPARES]):
        credit = entry.get('credit') or {} if isinstance(entry, dict) else {}
        try:
            image = _art_image(cfg, client, credit)
            data = encode_frame(render_art(image, credit))
            spares.append(_publish_frame(out, base_url, f'spare{index}', data, now))
        except Exception as exc:
            if index < len(previous):
                spares.append(previous[index])
            log.warning('lcd43b spare %d render failed: %s', index, exc)
    return spares