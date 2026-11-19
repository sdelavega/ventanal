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
from standalone_render import ART_CASES, WEATHER_CASES, art_case, weather_case, write_art_inputs  # noqa: E402

MEAN_LIMIT, NEAR, NEAR_SHARE = 2.0, 24, 0.99


def as_rgb565_expanded(image):
    """The server image as the board would hold it: truncated to RGB565, expanded back."""
    a = np.asarray(image.convert('RGB')).astype(np.uint16)
    r, g, b = a[..., 0] >> 3, a[..., 1] >> 2, a[..., 2] >> 3
    return np.stack([(r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2)], axis=-1).astype(np.int16)


# Art is resampled differently (Lanczos with fractional crop boxes on the
# server, integer crops and area averages on the board) and its backdrop is
# blurred at 1/8 scale, so detailed pieces differ pixel by pixel while looking
# the same (measured 2026-11-19: mean 3.6-7.1, 89-97 % within 24/255). This
# bound catches a wrong crop, backdrop, colour order or a missing credit.
ART_MEAN_LIMIT, ART_NEAR_SHARE = 9.0, 0.85


def main():
    board_dir = Path(sys.argv[1])
    if len(sys.argv) > 2 and sys.argv[2] == '--inputs':
        write_art_inputs(board_dir)  # what the board's host render reads
        return
    failed = False
    cases = [(name, weather_case, MEAN_LIMIT, NEAR_SHARE) for name in WEATHER_CASES]
    cases += [(name, art_case, ART_MEAN_LIMIT, ART_NEAR_SHARE) for name in ART_CASES]
    for name, render, mean_limit, near_share in cases:
        server = as_rgb565_expanded(render(name))
        board = np.asarray(Image.open(board_dir / f'{name}.ppm').convert('RGB')).astype(np.int16)
        diff = np.abs(server - board)
        mean = diff.mean()
        near = (diff.max(axis=2) <= NEAR).mean()
        ok = mean <= mean_limit and near >= near_share
        print(f'{name}: mean {mean:.3f} (<= {mean_limit}), within {NEAR}/255 {near * 100:.2f} % '
              f'(>= {near_share * 100:.1f} %) {"ok" if ok else "FAIL"}')
        if not ok:
            failed = True
            worst = diff.max(axis=2).astype(np.float32)
            Image.fromarray((worst * (255.0 / max(1.0, worst.max()))).astype(np.uint8)).save(
                board_dir / f'diff-{name}.png')
    sys.exit(1 if failed else 0)


if __name__ == '__main__':
    main()
