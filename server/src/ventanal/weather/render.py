# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Stephen de la Vega

"""Render weather layout A with patterned chrome and exact panel-colour ink."""

from datetime import datetime

import numpy as np
from PIL import Image, ImageDraw, ImageFilter

from ventanal.config import Config
from ventanal.manifest import Readout
from ventanal.palette import LCARS_MODERN, PANEL_INDEX
from ventanal.quantize import Canvas, Method
from ventanal.text import fit, font, renderable
from ventanal.weather.data import Alert, Weather, WMO_LONG, WMO_SHORT
from ventanal.weather.lcars import draw_frame, pill, sidebar_ink
from ventanal.weather.radar import Radar, clean_echoes

WEATHER_READOUT = Readout(1236, 30, 334, 136, 'lcars')
_RADAR_ORIGIN = (784, 234)
_RADAR_SIZE = (782, 462)


def _text(canvas, text, xy, px, colour, *, bold=False, anchor='ls', max_w=None):
    """Paste binary glyphs so every ink pixel is exact SOLID panel colour."""
    name = 'antonio-bold' if bold else 'antonio'
    face = font(name, px)
    text = renderable(text, name)
    if max_w is not None:
        text = fit(text, face, max_w)
    if not text:
        return
    left, top, right, bottom = face.getbbox(text, anchor=anchor)
    if right <= left or bottom <= top:
        return
    mask = Image.new('1', (right - left, bottom - top))
    draw = ImageDraw.Draw(mask)
    draw.text((-left, -top), text, font=face, anchor=anchor, fill=1)
    canvas.paste(Image.new('RGB', mask.size, colour),
                 (round(xy[0] + left), round(xy[1] + top)), Method.SOLID, mask)


def _location_marker(canvas, centre, colours, here_label="es"):
    """Keep the optional location caption and its backing inside the radar."""
    cx, cy = centre
    solid = canvas.region(Method.SOLID)
    # 17 px overall (was 23): black core and 1 px white contrast ring.
    solid.ellipse((cx - 8, cy - 8, cx + 8, cy + 8), fill=colours['white'])
    if here_label == 'off':
        solid.ellipse((cx - 7, cy - 7, cx + 7, cy + 7), fill=colours['black'])
        return
    label = 'YOU ARE HERE' if here_label == 'en' else 'ESTÁS AQUÍ'
    if renderable(label, 'antonio-bold') != label:
        raise ValueError('Antonio Bold must support ESTÁS AQUÍ, including Á and Í')
    face = font('antonio-bold', 26)
    left, top, right, bottom = face.getbbox(label, anchor='ls')
    padding = 6
    width, height = right - left + 2 * padding, bottom - top + 2 * padding
    cx, cy = centre
    x = max(0, min(round(cx - width / 2), canvas.rgb.width - width))
    # Account for Q's descender so the backing clears the marker and leader.
    y = cy - 18 - (bottom - top) - padding
    below = y < 0
    if below:
        y = cy + 18
    y = min(y, canvas.rgb.height - height)
    solid = canvas.region(Method.SOLID)
    solid.rounded_rectangle((x, y, x + width - 1, y + height - 1),
                            radius=height / 2, fill=colours['black'])
    edge = y if below else y + height - 1
    marker_edge = cy + 8 if below else cy - 8
    solid.line(((x + width // 2, edge), (cx, marker_edge)),
               fill=colours['black'], width=2)
    _text(canvas, label, (x + padding - left, y + padding - top),
          26, colours['white'], bold=True)
    solid.ellipse((cx - 8, cy - 8, cx + 8, cy + 8), fill=colours['white'])
    solid.ellipse((cx - 7, cy - 7, cx + 7, cy + 7), fill=colours['black'])


def render_weather(w: Weather, alerts: list[Alert], radar: Radar | None, lines,
                   place: str, coords: tuple[float, float], now: datetime, cfg: Config) -> Canvas:
    """Compose a 1600×1200 frame; radar inputs use the 782×462 inner size."""
    colours = {name: tuple(cfg.panel_rgb[index]) for name, index in PANEL_INDEX.items()}
    black, white, yellow, red = (colours[name] for name in ('black', 'white', 'yellow', 'red'))
    canvas = Canvas(fill=(0, 0, 0))
    pattern = canvas.region(Method.PATTERN)
    m = LCARS_MODERN
    code1 = f'{now.timetuple().tm_yday:03d}-{now:%H%M}'
    code2 = f'{w.sunrise:%H%M}-{w.sunset:%H%M}'
    segments = (
        (212, 118, 'sky', code1, black),
        (336, 252, 'orange', 'RADAR', black),
        (594, 66, 'light_gray', code2, black),
        (666, 322, 'gray', 'LONG RANGE', white),
    )
    draw_frame(canvas, [(y, h, colour) for y, h, colour, _, _ in segments])
    for y, h, colour, label, ink in segments:
        _text(canvas, label, (248, y + h - 12), 26, sidebar_ink(colour, cfg.panel_rgb), bold=True, anchor='rs')

    _text(canvas, 'ATMOSPHERIC CONDITIONS', (330, 148), 64, yellow, bold=True)
    lat, lon = coords
    zip5 = (cfg.zip or '')[:5]
    _text(canvas, f'{place} · {zip5} · {lat:.2f}N {abs(lon):.2f}W',
          (332, 194), 30, white, max_w=1238)
    _text(canvas, f'{w.current.temp:.0f}°', (330, 430), 220, white, bold=True, max_w=430)
    _text(canvas, WMO_LONG.get(w.current.code, 'Unknown').upper(),
          (340, 500), 44, yellow, bold=True, max_w=420)

    def clock(value):
        return value.strftime('%H:%M') if cfg.clock_24h else value.strftime('%I:%M %p').lstrip('0')

    wind_unit = 'MPH' if cfg.units == 'imperial' else 'KM/H'
    if w.rain_onset:
        if w.days:
            rain = f'RAIN {w.days[0].precip_prob}% AFTER {clock(w.rain_onset)}'
        else:
            rain = f'RAIN AFTER {clock(w.rain_onset)}'
    else:
        rain = 'NO RAIN EXPECTED TODAY'
    for i, label in enumerate((f'WIND {w.current.wind_dir} {w.current.wind_speed:.0f} {wind_unit}',
                               f'HUMIDITY {w.current.humidity}%', rain)):
        _text(canvas, label, (340, 560 + i * 46), 32, white, max_w=420)
    _text(canvas, f'SUNSET {clock(w.sunset)}', (340, 698), 32, yellow, max_w=420)

    pattern.rounded_rectangle((780, 230, 1569, 699), radius=28, fill=m['sky'])
    # Compose every map element locally, then clip RGB and method labels with
    # the same binary rounded mask; no map ink can escape onto the border.
    inner = Canvas(_RADAR_SIZE, fill=white, method=Method.SOLID)
    if radar is None:
        _text(inner, 'RADAR UNAVAILABLE', (391, 244), 40, red, anchor='ms', max_w=750)
    else:
        if radar.levels.shape != (462, 782):
            raise ValueError('radar.levels must have shape (462, 782)')
        map_draw = inner.region(Method.SOLID)
        for layer, width, colour in (('counties', 1, colours['blue']), ('states', 2, black)):
            for line in lines[layer]:
                if len(line) >= 2:
                    map_draw.line(line, fill=colour, width=width)
        # Paint low-to-high intensity, outlining each area before its fill.
        # Nested yellow/red cells retain visible boundaries within green echoes.
        levels = clean_echoes(radar.levels)
        masks = [(name, Image.fromarray((levels == level).astype(np.uint8) * 255))
                 for level, name in ((1, 'green'), (2, 'yellow'), (3, 'red'))]
        for name, mask in masks:
            inner.paste(Image.new('RGB', _RADAR_SIZE, black), (0, 0),
                        Method.SOLID, mask.filter(ImageFilter.MaxFilter(5)))
            inner.paste(Image.new('RGB', _RADAR_SIZE, colours[name]), (0, 0), Method.SOLID, mask)
        _location_marker(inner, (396, 236), colours, cfg.here_label)
        valid = radar.valid.astimezone(now.tzinfo)
        caption = f'NEXRAD COMPOSITE · {clock(valid)} · 150 MI'
        width = int(font('antonio', 24).getlength(caption)) + 24
        map_draw.rounded_rectangle((8, 420, 8 + width, 457), radius=12, fill=black)
        _text(inner, caption, (20, 449), 24, white, max_w=726)
    clip = Image.new('1', _RADAR_SIZE)
    ImageDraw.Draw(clip).rounded_rectangle((0, 0, 781, 461), radius=24, fill=1)
    canvas.paste(inner.rgb, _RADAR_ORIGIN, Method.SOLID, clip)
    canvas.method.paste(inner.method, _RADAR_ORIGIN, clip)

    pattern.rectangle((330, 748, 1569, 751), fill=m['sky'])
    for i, day in enumerate(w.days[:7]):
        cx = 410 + i * 178
        _text(canvas, day.date.strftime('%a').upper(), (cx, 812), 40, yellow, bold=True, anchor='ms')
        _text(canvas, str(day.hi), (cx, 890), 66, white, bold=True, anchor='ms')
        _text(canvas, str(day.lo), (cx, 940), 40, white, anchor='ms')
        _text(canvas, WMO_SHORT.get(day.code, 'UNKNOWN'), (cx, 976), 24, white,
              anchor='ms', max_w=168)
        pill(pattern, (cx - 65, 990, 130, 22), m['gray'], True, True)
        chance = max(0, min(100, day.precip_prob))
        pill(pattern, (cx - 65, 990, 130 * chance / 100, 22), m['sky'], True, True)

    advisory = 'NO ACTIVE ADVISORIES'
    if alerts:
        advisory = f'ADVISORY · {alerts[0].event.upper()}'
        if alerts[0].ends is not None:
            ends = alerts[0].ends.astimezone(now.tzinfo)
            advisory += f' UNTIL {ends:%a} {clock(ends)}'
    _text(canvas, advisory, (330, 1060), 32, red if alerts else white, bold=True, max_w=1240)
    return canvas
