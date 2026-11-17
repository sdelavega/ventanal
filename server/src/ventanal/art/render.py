# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Stephen de la Vega

"""Compose museum artwork, soft shade and fitted white credits."""

from math import ceil

from PIL import Image, ImageDraw, ImageFilter

from ventanal.art.compose import compose
from ventanal.art.sources import Candidate
from ventanal.manifest import Readout
from ventanal.quantize import Canvas, Method
from ventanal.text import fit, font, renderable

ART_READOUT = Readout(1020, 0, 580, 104, 'shade')


def render_art(img: Image.Image, c: Candidate) -> tuple[Canvas, dict]:
    backdrop = compose(img)
    canvas = Canvas()
    canvas.paste(backdrop, (0, 0), Method.DIFFUSE)

    title_font, secondary_font = font('inter-bold', 17), font('inter', 15)
    title = renderable(c.title, 'inter-bold') or 'Untitled'
    fields = [renderable(value, 'inter') for value in (c.artist, c.date, c.museum)]
    available = 1544
    title = fit(title, title_font, available * .6)
    remaining = available - ceil(title_font.getlength(title))
    # Preserve the artist, dropping museum and then date before fitting it.
    while True:
        secondary = ' · '.join(value for value in fields if value)
        secondary = '   ' + secondary if secondary else ''
        if secondary_font.getlength(secondary) <= remaining or len(fields) == 1:
            break
        fields.pop()
    secondary = fit(secondary, secondary_font, remaining)
    text_width = title_font.getlength(title) + secondary_font.getlength(secondary)
    # The approved credit shade stays on the server; firmware shades its readout.
    mask = Image.new('L', backdrop.size)
    shade = ImageDraw.Draw(mask)
    shade.rounded_rectangle((0, 1138, 28 + text_width + 40, 1200), 30, fill=150)
    canvas.paste(Image.new('RGB', backdrop.size), (0, 0), Method.DIFFUSE,
                 mask.filter(ImageFilter.GaussianBlur(14)))
    draw = canvas.region(Method.SOLID)
    draw.text((28, 1172), title, font=title_font, fill='white', anchor='ls')
    draw.text((28 + title_font.getlength(title), 1172), secondary,
              font=secondary_font, fill='white', anchor='ls')
    credit = {'title': title, 'secondary': secondary, 'artist': c.artist,
              'date': c.date, 'museum': c.museum, 'source': c.source,
              'object_id': c.object_id, 'x': 28, 'y': 1172, 'w': ceil(text_width), 'h': 17}
    return canvas, credit
