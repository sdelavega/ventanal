<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright (C) 2026 Stephen de la Vega -->
# Readout fonts

Generated from the repository's bundled Google Fonts TTFs using the Adafruit-GFX
`fontconvert.c` shipped with InkplateLibrary 11.2.1 (DPI 141), ASCII 32–126:

```
fontconvert server/src/ventanal/fonts/antonio/Antonio-Bold.ttf 18 32 126
fontconvert server/src/ventanal/fonts/antonio/Antonio-Regular.ttf 18 32 126
```

Antonio H glyphs remain exactly 30 pixels high; these fonts are unchanged.
Inter is regenerated with `firmware/tools/generate_inter.py`, passing the path to
InkplateLibrary's `extras/fontconvert/fontconvert.c`. It uses FreeType monochrome
rasterization without hinting at 16.5 and 20.5 px (72 DPI and a half-point
adjustment). `Inter_Medium12cap7b` has an H bitmap exactly 12 pixels high and is
used by the shade readout. `Inter_Medium15cap7b` retains its exact 15-pixel H for
the waiting screen, which wraps long URLs.

Headers add the project licence and copyright comments and `#pragma once`.
The original SIL Open Font Licences are included alongside the generated data.
The renderer draws middle dots itself because GFX's ASCII fonts do not decode UTF-8.
All text uses natural glyph widths. Art drops the weekday and then the date when
needed; no board text is horizontally condensed. The soft shade background and the
weather gold block come from the cached frame and are preserved.
