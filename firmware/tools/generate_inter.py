# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Stephen de la Vega

"""Regenerate the 12- and 15-pixel-cap Inter fonts using Inkplate's fontconvert source.

Usage: python generate_inter.py /path/to/InkplateLibrary/extras/fontconvert/fontconvert.c
Requires a C compiler and pkg-config freetype2. Temporary files self-clean.
"""

from pathlib import Path
import shlex
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[2]
converter = Path(sys.argv[1]).resolve()
source = converter.read_text()
for old, new in (
    ('#define DPI 141', '#define DPI 72'),
    ('size << 6', '(size << 6) - 32'),
    ('FT_LOAD_TARGET_MONO', 'FT_LOAD_TARGET_MONO | FT_LOAD_NO_HINTING'),
):
    if source.count(old) != 1:
        raise ValueError(f'Unexpected converter source: {old}')
    source = source.replace(old, new)
with tempfile.TemporaryDirectory(prefix='ventanal-font-') as directory:
    tmp = Path(directory)
    (tmp / 'fontconvert.c').write_text(source)
    flags = shlex.split(subprocess.check_output(
        ['pkg-config', '--cflags', '--libs', 'freetype2'], text=True))
    subprocess.run(['cc', str(tmp / 'fontconvert.c'), '-I' + str(converter.parent),
                    *flags, '-o', str(tmp / 'fontconvert')], check=True)
    header = ('// SPDX-License-Identifier: GPL-3.0-or-later\n'
              '// Copyright (C) 2026 Stephen de la Vega\n#pragma once\n')
    fonts = []
    for size, cap in ((17, 12), (21, 15)):
        data = subprocess.check_output([
            str(tmp / 'fontconvert'),
            str(root / 'server/src/ventanal/fonts/inter/Inter-Medium.ttf'),
            str(size), '32', '126'], text=True)
        fonts.append(data.replace(f'Inter_Medium{size}pt7b', f'Inter_Medium{cap}cap7b'))
(root / 'firmware/ventanal/fonts/Inter-Medium.h').write_text(header + '\n'.join(fonts))
