# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Stephen de la Vega

"""Fill the panel while retaining unusually shaped artwork in its entirety."""

from PIL import Image, ImageEnhance, ImageFilter, ImageOps


def compose(img: Image.Image, size=(1600, 1200)) -> Image.Image:
    img = img.convert('RGB')
    cover = ImageOps.fit(img, size, method=Image.Resampling.LANCZOS)
    ratio = (img.width / img.height) / (4 / 3)
    if 1 / 1.15 <= ratio <= 1.15:
        return cover
    background = ImageEnhance.Brightness(cover.filter(ImageFilter.GaussianBlur(60 * size[0] / 1600))).enhance(0.55)
    foreground = ImageOps.contain(img, size, method=Image.Resampling.LANCZOS)
    background.paste(foreground, ((size[0] - foreground.width) // 2,
                                  (size[1] - foreground.height) // 2))
    return background
