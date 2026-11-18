# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Stephen de la Vega

"""Native 800×480 weather: A2 sidebar, large radar and bounded advisory."""

import logging

import numpy as np
from ventanal.devices.lcd_draw import LCDDraw, rounded_mask
from PIL import Image, ImageFilter

from ventanal.devices.lcd_lcars import LCD_COLOURS, draw_lcd_frame
from ventanal.quantize import Canvas, Method
from ventanal.text import fit, font, renderable
from ventanal.weather.data import WMO_LONG, WMO_SHORT
from ventanal.weather.radar import clean_echoes

log = logging.getLogger(__name__)
MAP_SOURCE_SIZE = (782, 462)
RADAR_BOX = (366, 80, 786, 328)
SECTIONS = [(60, 150, 'sky', 'NOW'), (214, 96, 'gold', 'RADAR'),
            (314, 112, 'light_gray', 'LONG RANGE')]


def _clock(value, cfg):
    return (value.strftime('%H:%M') if cfg.clock_24h else
            value.strftime('%I:%M %p').lstrip('0'))


def _text(draw, label, xy, size, colour='white', *, bold=False,
          anchor='ls', width=None):
    name = 'antonio-bold' if bold else 'antonio'
    face = font(name, size)
    label = renderable(label, name)
    if width is not None:
        label = fit(label, face, width)
    draw.text(xy, label, font=face, fill=LCD_COLOURS.get(colour, colour), anchor=anchor)


def weather_layout(alerts, cfg, now) -> dict:
    """Plan the exact advisory glyph bounds, independently of the forecast.

    Wrap at most three lines; fit the remaining text on the last line.
    The reserved forecast includes the rain bars, ending at y=413.
    """
    advisory = 'NO ACTIVE ADVISORIES'
    if alerts:
        advisory = f'ADVISORY · {alerts[0].event.upper()}'
        if alerts[0].ends is not None:
            ends = alerts[0].ends.astimezone(now.tzinfo)
            advisory += f' UNTIL {ends:%a} {_clock(ends, cfg)}'
    face = font('antonio-bold', 12)
    words = renderable(advisory, 'antonio-bold').split()
    lines = []
    while words and len(lines) < 2:
        line = words.pop(0)
        while words and face.getlength(line + ' ' + words[0]) <= 636:
            line += ' ' + words.pop(0)
        lines.append(fit(line, face, 636))
    if words:
        lines.append(fit(' '.join(words), face, 636))
    positions = [(150, 430 + i * 12) for i in range(len(lines))]
    bounds = [face.getbbox(line, anchor='ls') for line in lines]
    bbox = (min(x+b[0] for (x, y), b in zip(positions, bounds)),
            min(y+b[1] for (x, y), b in zip(positions, bounds)),
            max(x+b[2] for (x, y), b in zip(positions, bounds)),
            max(y+b[3] for (x, y), b in zip(positions, bounds)))
    return {'forecast_bbox': (150, 334, 786, 413), 'advisory_bbox': bbox,
            'advisory_lines': list(zip(lines, positions)), 'radar_bbox': RADAR_BOX}


def _radar_image(radar, lines, cfg, now):
    """Composite at the existing map scale to preserve echo cleanup/markers."""
    colours = {'black': (0, 0, 0), 'white': (255, 255, 255),
               'blue': (60, 91, 208), 'green': (45, 107, 58),
               'yellow': (232, 207, 58), 'red': (184, 31, 31)}
    inner = Canvas(MAP_SOURCE_SIZE, fill=colours['white'], method=Method.SOLID)
    d = LCDDraw(inner.rgb)
    if radar is None:
        _text(LCDDraw(inner.rgb), 'RADAR UNAVAILABLE', (391, 244), 32,
              'red', anchor='ms', width=750)
    else:
        if radar.levels.shape != (462, 782):
            raise ValueError('radar.levels must have shape (462, 782)')
        for layer, width, colour in [('counties', 1, colours['blue']),
                                      ('states', 2, colours['black'])]:
            for line in lines.get(layer, []):
                if len(line) >= 2:
                    d.line(line, fill=colour, width=width)
        levels = clean_echoes(radar.levels)
        for level, name in [(1, 'green'), (2, 'yellow'), (3, 'red')]:
            mask = Image.fromarray((levels == level).astype(np.uint8) * 255)
            inner.paste(Image.new('RGB', MAP_SOURCE_SIZE, colours['black']),
                        (0, 0), Method.SOLID, mask.filter(ImageFilter.MaxFilter(5)))
            inner.paste(Image.new('RGB', MAP_SOURCE_SIZE, colours[name]),
                        (0, 0), Method.SOLID, mask)
        location_marker(inner.rgb, (396, 236), cfg.here_label)
        valid = radar.valid.astimezone(now.tzinfo)
        caption = (f'NEXRAD COMPOSITE · {_clock(valid, cfg)} · '
                   f'{cfg.radar_radius_miles:g} MI')
        face = font('antonio', 24)
        caption = fit(caption, face, 726)
        d.rounded_rectangle((8, 420, 32 + face.getlength(caption), 457),
                            radius=12, fill=colours['black'])
        _text(LCDDraw(inner.rgb), caption, (20, 449), 24)
    return inner.rgb


def location_marker(image, centre, here_label):
    """LCD marker uses AA text and curves without touching the e-paper helper."""
    cx, cy = centre
    d = LCDDraw(image)
    if here_label != 'off':
        label = 'YOU ARE HERE' if here_label == 'en' else 'ESTÁS AQUÍ'
        face = font('antonio-bold', 26)
        left, top, right, bottom = face.getbbox(label, anchor='ls')
        width, height = right-left+12, bottom-top+12
        x = max(0, min(round(cx-width/2), image.width-width))
        y = cy-18-(bottom-top)-6
        below = y < 0
        if below:
            y = cy+18
        y = min(y, image.height-height)
        d.rounded_rectangle((x, y, x+width-1, y+height-1),
                            radius=height/2, fill='black')
        d.line(((x+width//2, y if below else y+height-1),
                (cx, cy+8 if below else cy-8)), fill='black', width=2)
        _text(d, label, (x+6-left, y+6-top), 26, bold=True)
    d.ellipse((cx-8, cy-8, cx+8, cy+8), fill='white')
    d.ellipse((cx-7, cy-7, cx+7, cy+7), fill='black')


def render_lcd_weather(w, alerts, radar, cfg, now, *, lines=None,
                       place=None, coords=None) -> Image.Image:
    """Render native LCD content, leaving the clock entirely to the board."""
    image = Image.new('RGB', (800, 480))
    draw_lcd_frame(image, SECTIONS)
    d = LCDDraw(image)
    _text(d, 'ATMOSPHERIC CONDITIONS', (150, 56), 30, 'gold', bold=True)
    location = place or cfg.place_name or ''
    coords = coords or (cfg.latitude, cfg.longitude)
    lat, lon = coords
    if lat is not None and lon is not None:
        location += f' · {abs(lat):.2f}{"N" if lat >= 0 else "S"} {abs(lon):.2f}{"E" if lon >= 0 else "W"}'
    _text(d, location.upper().strip(' ·'), (151, 74), 13, 'light_gray', width=635)
    _text(d, f'{w.current.temp:.0f}°', (148, 190), 108, bold=True, width=204)
    _text(d, WMO_LONG.get(w.current.code, 'Unknown').upper(), (150, 222),
          24, 'gold', bold=True, width=204)
    unit = 'MPH' if cfg.units == 'imperial' else 'KM/H'
    rain = 'NO RAIN EXPECTED TODAY'
    if w.rain_onset:
        chance = f'{w.days[0].precip_prob}% ' if w.days else ''
        rain = f'RAIN {chance}AFTER {_clock(w.rain_onset, cfg)}'
    labels = [f'WIND {w.current.wind_dir} {w.current.wind_speed:.0f} {unit}',
              f'HUMIDITY {w.current.humidity}%', rain,
              f'SUNSET {_clock(w.sunset, cfg)}']
    for i, label in enumerate(labels):
        _text(d, label, (150, 246 + i*24), 16,
              'gold' if i == 3 else 'white', width=204)
    x0, y0, x1, y1 = RADAR_BOX
    map_image = _radar_image(radar, lines or {}, cfg, now).resize(
        (x1-x0-4, y1-y0-4), Image.Resampling.LANCZOS)
    clip = rounded_mask(map_image.size, 12)
    d.rounded_rectangle((x0, y0, x1-1, y1-1), radius=14, fill='white')
    image.paste(map_image, (x0+2, y0+2), clip)
    d.line((150, 331, 785, 331), fill=LCD_COLOURS['gray'])
    for i, day in enumerate(w.days[:7]):
        cx = 195 + i*91
        _text(d, day.date.strftime('%a').upper(), (cx, 351), 19, 'gold', bold=True, anchor='ms')
        _text(d, str(day.hi), (cx, 377), 27, bold=True, anchor='ms', width=82)
        _text(d, str(day.lo), (cx, 395), 17, 'light_gray', anchor='ms', width=82)
        _text(d, WMO_SHORT.get(day.code, 'UNKNOWN'), (cx, 407), 12,
              anchor='ms', width=82)
        d.rounded_rectangle((cx-30, 410, cx+29, 413), radius=2, fill=LCD_COLOURS['gray'])
        chance = max(0, min(100, day.precip_prob))
        if chance:
            d.rounded_rectangle((cx-30, 410, cx-31 + max(1, round(60*chance/100)), 413),
                                radius=2, fill=LCD_COLOURS['sky'])
    layout = weather_layout(alerts, cfg, now)
    for label, xy in layout['advisory_lines']:
        _text(d, label, xy, 12, 'red' if alerts else 'light_gray', bold=True)
    return image


def weather_frame(cfg, now, client) -> Image.Image:
    """Use the e-paper data providers and their independent failure handling."""
    from ventanal import jobs
    lat, lon, place = jobs.geocode(cfg, client, cfg.cache_dir)
    weather = jobs.fetch_weather(lat, lon, cfg.units, client, now=now)
    try:
        alerts = jobs.fetch_alerts(lat, lon, client, cfg.nws_contact)
    except Exception as exc:
        log.warning('LCD weather alerts unavailable: %s', exc)
        alerts = []
    bbox = jobs.bbox_around(lat, lon, cfg.radar_radius_miles, 782/462)
    radar = None
    if cfg.radar == 'iem':
        try:
            radar = jobs.fetch_iem(bbox, MAP_SOURCE_SIZE, client, cfg.cache_dir)
        except Exception as exc:
            log.warning('LCD weather radar unavailable: %s', exc)
    try:
        lines = jobs.basemap(bbox, MAP_SOURCE_SIZE, cfg.cache_dir, client)
    except Exception as exc:
        log.warning('LCD weather basemap unavailable: %s', exc)
        lines = {}
    return render_lcd_weather(weather, alerts, radar, cfg, now, lines=lines,
                              place=place, coords=(lat, lon))
