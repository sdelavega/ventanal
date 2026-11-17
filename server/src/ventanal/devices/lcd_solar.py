# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Stephen de la Vega

"""Native solar content in the shared 800×480 LCD sidebar frame."""

from datetime import datetime, timezone

from PIL import Image, ImageDraw

from ventanal.devices.lcd_lcars import LCD_COLOURS, draw_lcd_frame
from ventanal.solar.conditions import conditions, grid_square
from ventanal.solar.data import forecast_days
from ventanal.solar.render import (
    alert_fit, alert_summary, crop_sun, forecast_label, integer_k, k_color,
    kp_notation,
)
from ventanal.text import fit, font, renderable

SECTIONS = [(60, 145, 'sky', 'SOLAR'), (209, 113, 'gold', 'BANDS'),
            (326, 54, 'light_gray', 'KP'), (384, 40, 'violet', '3-DAY')]


def _text(draw, label, xy, size=14, colour='white', *, bold=False,
          width=None, anchor='ls'):
    name = 'antonio-bold' if bold else 'antonio'
    face = font(name, size)
    label = renderable(str(label).upper(), name)
    if width is not None:
        label = fit(label, face, width)
    draw.text(xy, label, font=face, fill=LCD_COLOURS.get(colour, colour),
              anchor=anchor)


def _k_colour(k):
    return {'green': 'green', 'yellow': 'gold', 'red': 'red'}[k_color(k)]


def render_lcd_solar(data, cfg, now) -> Image.Image:
    """Render solar data at panel size; the board owns the top-bar clock.

    ``now`` is the data-fetch time, shown in UTC in the content header.
    Coordinates must already be resolved by the caller, as for weather.
    """
    image = Image.new('RGB', (800, 480))
    draw_lcd_frame(image, SECTIONS)
    d = ImageDraw.Draw(image)
    _text(d, 'SOLAR CONDITIONS', (150, 56), 30, 'gold', bold=True)
    grid = grid_square(cfg.latitude, cfg.longitude)
    _text(d, f'{cfg.ham_call_sign} · {grid} · '
          f'{now.astimezone(timezone.utc):%d %b %Y %H:%M} UTC · NOAA SWPC · NASA SDO',
          (151, 74), 13, 'light_gray', width=635)

    _text(d, 'SOLAR FLUX (SFI)', (150, 96), 14, 'light_gray')
    _text(d, f'{data.sfi:.0f}', (148, 178), 90, bold=True, width=175)
    for label, value, x, y, colour in [
        ('SUNSPOTS', data.ssn, 340, 96, 'white'),
        ('X-RAY', data.xray, 448, 96, 'gold'),
        ('K-INDEX', integer_k(data.k), 340, 153, _k_colour(integer_k(data.k))),
        ('A-INDEX', data.a, 448, 153, 'white'),
    ]:
        _text(d, label, (x, y), 13, 'light_gray')
        _text(d, value, (x, y+32), 34, colour, bold=True, width=95)
    est = conditions(data.sfi, data.k, data.a, data.xray)
    _text(d, f'FIELD {est.field} · EST. NOISE {est.noise} · MUF ≈ {est.muf} MHZ',
          (150, 201), 14, 'sky', width=400)

    _text(d, 'DAY', (326, 218), 13, 'light_gray', anchor='ms')
    _text(d, 'NIGHT', (462, 218), 13, 'light_gray', anchor='ms')
    for i, (band, day, night) in enumerate(est.bands):
        y = 224 + i*21
        _text(d, band, (150, y+14), 17, 'gold', bold=True, width=100)
        for x, rating in [(270, day), (406, night)]:
            colour = {'Good': 'green', 'Fair': 'gold', 'Poor': 'red'}[rating]
            d.rounded_rectangle((x, y, x+112, y+17), radius=9,
                                fill=LCD_COLOURS[colour])
            _text(d, rating, (x+56, y+13), 13, 'black', bold=True, anchor='ms')
    _text(d, 'HF CONDITIONS, MUF AND NOISE ARE ESTIMATES',
          (150, 322), 12, 'gold', width=400)

    sun = crop_sun(data.sun).resize((186, 186), Image.Resampling.LANCZOS)
    mask = Image.new('L', sun.size)
    ImageDraw.Draw(mask).ellipse((0, 0, 185, 185), fill=255)
    image.paste(sun, (582, 84), mask)
    _text(d, 'SUN TODAY · NASA SDO 171 Å', (675, 286), 13, 'violet',
          anchor='ms', width=218)
    current = data.scales.get('0', {})
    scales = ' · '.join(f"{key}{current.get(key, {}).get('Scale') or '0'}"
                        for key in ('R', 'S', 'G'))
    _text(d, f'NOAA SCALES {scales} (NOW)', (675, 305), 13,
          anchor='ms', width=218)

    # Reserve a full-width chart; threshold is drawn over all eight bars.
    x, y, width, height = 172, 330, 612, 23
    for tick in (0, 9):
        ty = y + height - tick/9*height
        d.line((x, ty, x+width, ty), fill=LCD_COLOURS['gray'])
        _text(d, tick, (164, ty+4), 11, 'light_gray', anchor='rs')
    step = width/8
    for i, (stamp, k) in enumerate(data.kp[-8:]):
        bx = round(x+i*step)
        bar_h = max(2, min(9, max(0, k))/9*height)
        d.rounded_rectangle((bx, y+height-bar_h, bx+52, y+height),
                            radius=2, fill=LCD_COLOURS[_k_colour(k)])
        _text(d, stamp[11:13], (bx+26, 365), 11, anchor='ms')
    threshold = y + height - 5/9*height
    d.line((x, threshold, x+width, threshold), fill=LCD_COLOURS['gold'])
    _text(d, 'PLANETARY KP · 3-HOUR INTERVALS · UTC', (150, 375), 11, 'light_gray')
    _text(d, 'G1 (KP 5)', (785, 375), 11, 'gold', anchor='rs')

    for i, (date, k, r, probability, g) in enumerate(forecast_days(data)):
        bx = 150+i*214
        try:
            date = datetime.fromisoformat(date).strftime('%a %d')
        except (ValueError, TypeError):
            date = 'UNKNOWN'
        _text(d, date, (bx, 394), 18, 'gold', bold=True, width=204)
        _text(d, f'KP MAX {kp_notation(k)} · {forecast_label(k, g)}',
              (bx, 411), 15, width=204)
        _text(d, f'R1–R2 {r if r is not None else "?"}% · '
              f'S1 {probability if probability is not None else "?"}%',
              (bx, 427), 13, 'light_gray', width=204)
    label, message = alert_summary(data.alerts)
    if label:
        _text(d, label, (150, 450), 16, 'gold', bold=True, width=72)
        message, size = alert_fit(message, 550)
        _text(d, message, (235, 450), size)
    else:
        _text(d, message, (150, 450), 14, 'light_gray', width=635)
    return image
