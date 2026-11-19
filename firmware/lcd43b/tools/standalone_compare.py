# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Stephen de la Vega
"""Hold the board's standalone screens to the server's pixels.

Renders each case with the server (tools/standalone_render.py), reduces it to
RGB565 like the frame encoder, and compares it with the board's host render
(test/build/render_local_screens). Passing: mean absolute difference <= 2.0
(8-bit channels) and >= 99.0 % of pixels within 24/255 on every channel. A
failing case writes BOARD_DIR/diff-<case>.png (white = worst).

Run with server/.venv/bin/python firmware/lcd43b/tools/standalone_compare.py BOARD_DIR
"""
import sys
from pathlib import Path

import numpy as np
from PIL import Image

sys.path.insert(0, str(Path(__file__).parent))
from standalone_render import WEATHER_CASES, weather_case  # noqa: E402

MEAN_LIMIT, NEAR, NEAR_SHARE = 2.0, 24, 0.99


def as_rgb565_expanded(image):
    """The server image as the board would hold it: truncated to RGB565, expanded back."""
    a = np.asarray(image.convert('RGB')).astype(np.uint16)
    r, g, b = a[..., 0] >> 3, a[..., 1] >> 2, a[..., 2] >> 3
    return np.stack([(r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2)], axis=-1).astype(np.int16)


def main():
    board_dir = Path(sys.argv[1])
    failed = False
    for name in WEATHER_CASES:
        server = as_rgb565_expanded(weather_case(name))
        board = np.asarray(Image.open(board_dir / f'{name}.ppm').convert('RGB')).astype(np.int16)
        diff = np.abs(server - board)
        mean = diff.mean()
        near = (diff.max(axis=2) <= NEAR).mean()
        ok = mean <= MEAN_LIMIT and near >= NEAR_SHARE
        print(f'{name}: mean {mean:.3f} (<= {MEAN_LIMIT}), within {NEAR}/255 {near * 100:.2f} % '
              f'(>= {NEAR_SHARE * 100:.1f} %) {"ok" if ok else "FAIL"}')
        if not ok:
            failed = True
            worst = diff.max(axis=2).astype(np.float32)
            Image.fromarray((worst * (255.0 / max(1.0, worst.max()))).astype(np.uint8)).save(
                board_dir / f'diff-{name}.png')
    sys.exit(1 if failed else 0)


if __name__ == '__main__':
    main()
