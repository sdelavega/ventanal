# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Stephen de la Vega

"""Native LCD sidebar chrome, with a black gap for the board's live clock."""

from PIL import Image, ImageDraw

from ventanal.text import fit, font
from ventanal.weather.lcars import elbow

TOPBAR_H = 26
CLOCK_GAP = (548, 0, 204, 26)
LCD_COLOURS = {
    'orange': '#ff7700', 'gold': '#ffaa00', 'violet': '#ddbbff',
    'sky': '#aaaaff', 'gray': '#666688', 'light_gray': '#9996ba',
    'red': '#dd4444', 'green': '#5fae4f',
}


def draw_lcd_frame(canvas: Image.Image,
                   sections: list[tuple[int, int, str, str]]) -> None:
    """Draw A2's elbows, labelled sidebar and bars on an 800×480 canvas."""
    if canvas.size != (800, 480) or canvas.mode != 'RGB':
        raise ValueError('LCD frame must be RGB 800x480')
    d = ImageDraw.Draw(canvas)
    orange = LCD_COLOURS['orange']
    elbow(d, 0, 0, 530, 56, TOPBAR_H, 100, 44, 30, orange, False)
    d.rectangle((534, 0, 547, 25), fill=LCD_COLOURS['gold'])
    d.rounded_rectangle((752, 0, 799, 25), radius=13, fill=orange)
    d.rectangle((752, 0, 775, 25), fill=orange)
    elbow(d, 0, 430, 800, 50, 22, 100, 44, 28, orange, True)
    face = font('antonio-bold', 13)
    for y, h, colour, label in sections:
        d.rectangle((0, y, 99, y + h - 1), fill=LCD_COLOURS[colour])
        d.text((92, y + h - 6), fit(label, face, 86), font=face,
               fill='black', anchor='rs')
    x, y, w, h = CLOCK_GAP
    d.rectangle((x, y, x+w-1, y+h-1), fill='black')
