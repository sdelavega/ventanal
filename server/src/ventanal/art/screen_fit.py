# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Stephen de la Vega
"""Palette-relative screen fitness. No image processing runs in HTTP handlers.

Loss = 65% viewing fidelity (mean Oklab distance / .20), 25% gamut
(chroma-weighted outside share times normalized linear hull distance), and
10% lost tone (mean lightness excursion / .20). Each loss is clipped to 1.
The threshold is calibrated against the live colour-eligible seasonal pool.
"""

from dataclasses import dataclass
import hashlib
import json
import logging
import time
import numpy as np
from PIL import ImageFilter
from ventanal.art.compose import compose
from ventanal.palette import to_oklab
from ventanal.quantize import (
    Canvas,
    Method,
    quantize,
    simulate,
    _to_linear,
    _hull_planes,
)

# Live calibrated colour-eligible pool median is approximately 88 (see docs/art-sources.md).
THRESHOLD = 88.0
VERSION = "screen-fit-v1"
BATCH_SIZE = 8
BUDGET_SECONDS = 20.0
# Persist cooldown before starting work so hard worker termination backs off too.
RETRY_BASE_SECONDS = 300
RETRY_MAX_SECONDS = 86400
log = logging.getLogger(__name__)


def fingerprint(palette):
    return hashlib.sha256(
        json.dumps([VERSION, np.asarray(palette).tolist()]).encode()
    ).hexdigest()


@dataclass(frozen=True)
class Assessment:
    score: float
    fidelity: float
    gamut: float
    tonal: float


def preview(image, palette):
    source = compose(image, (400, 300))
    canvas = Canvas(source.size, method=Method.DIFFUSE)
    canvas.paste(source, (0, 0), Method.DIFFUSE)
    return source, simulate(quantize(canvas, palette), palette)


def assess(image, palette):
    source, output = preview(image, palette)
    lab = to_oklab(np.asarray(source))
    viewed = [
        to_oklab(np.asarray(im.filter(ImageFilter.GaussianBlur(2))))
        for im in (source, output)
    ]
    fidelity = float(np.linalg.norm(viewed[0] - viewed[1], axis=-1).mean())
    panel = to_oklab(np.asarray(palette))
    vertices = _to_linear(panel)
    linear = _to_linear(lab)
    planes = _hull_planes(vertices)
    if len(planes):
        distance = np.maximum((linear @ planes[:, :3].T - planes[:, 3]).max(axis=-1), 0)
    else:
        # Degenerate palettes: distance to the reachable segment, not an empty hull.
        a, b = vertices[np.argmin(panel[:, 0])], vertices[np.argmax(panel[:, 0])]
        direction = b - a
        t = np.clip(
            ((linear - a) * direction).sum(-1)
            / max(float(direction @ direction), 1e-12),
            0,
            1,
        )
        distance = np.linalg.norm(linear - (a + t[..., None] * direction), axis=-1)
    chroma = np.linalg.norm(lab[..., 1:], axis=-1)
    gamut = float(
        (chroma * (distance > 1e-6) * np.minimum(1, distance / 0.25)).sum()
        / max(float(chroma.sum()), 1e-12)
    )
    excursion = np.maximum(panel[:, 0].min() - lab[..., 0], 0) + np.maximum(
        lab[..., 0] - panel[:, 0].max(), 0
    )
    tonal = float(excursion.mean())
    loss = 0.65 * min(1, fidelity / 0.20) + 0.25 * gamut + 0.10 * min(1, tonal / 0.20)
    return Assessment(
        round(float(np.clip(100 * (1 - loss), 0, 100)), 2), fidelity, gamut, tonal
    )


def batch(cfg):
    """A killable worker bounds ALL download/quantizer work, including cold JIT.

    SQLite commits each completed work; interrupted work remains stale for retry.
    At most eight artworks and twenty seconds are spent per tick.
    """
    from ventanal.art.image_cache import configure
    configure(cfg)
    import subprocess
    import sys

    try:
        subprocess.run(
            [
                sys.executable,
                "-m",
                __name__,
                str(cfg.cache_dir),
                json.dumps(cfg.panel_rgb),
                json.dumps(cfg.museums),
            ],
            timeout=BUDGET_SECONDS,
            check=False,
            stdout=subprocess.DEVNULL,
            stderr=None,  # Sanitized source warnings reach the service journal.
        )
    except subprocess.TimeoutExpired:
        log.warning(
            "Screen-fit worker budget exhausted; interrupted artwork backed off"
        )


def _worker(cache, palette, museums):
    from pathlib import Path
    from contextlib import closing
    import sqlite3
    import httpx
    from ventanal.art.pool import ArtPool
    from ventanal.art.sources import fetch_image, failure_info

    # Use the same production kernel; persist its optional compilation across
    # short-lived workers in the service-writable image cache, not /opt.
    from ventanal import quantize as q

    if q.njit is not None:
        import numba

        numba.config.CACHE_DIR = str(Path(cache) / "screen-fit-jit")
        q._diffuse = q.njit(cache=True)(getattr(q._diffuse, "py_func", q._diffuse))
    pool = ArtPool(Path(cache) / "art.sqlite3")
    with closing(sqlite3.connect(pool.db_path)) as db:
        rows = db.execute(
            """SELECT source, object_id, fit_failures FROM artworks
               WHERE (fit_score IS NULL OR fit_palette IS NULL OR fit_palette != ?)
                 AND (fit_retry_after IS NULL OR fit_retry_after <= ?)
               ORDER BY COALESCE(fit_attempt,0), source, object_id""",
            (fingerprint(palette), time.time()),
        ).fetchall()
    with httpx.Client(timeout=8, follow_redirects=True) as client:
        count = 0
        for source, ident, failures in rows:
            if source not in museums:
                continue
            count += 1
            attempted = time.time()
            delay = min(RETRY_BASE_SECONDS * 2 ** min(failures, 9), RETRY_MAX_SECONDS)
            with closing(sqlite3.connect(pool.db_path)) as db, db:
                db.execute(
                    """UPDATE artworks SET fit_attempt=?, fit_failures=fit_failures+1,
                       fit_retry_after=? WHERE source=? AND object_id=?""",
                    (attempted, attempted + delay, source, ident),
                )
            c = pool.get(source, ident)
            try:
                # jobs.fetch_candidate is the source of truth for museum cache
                # dispatch. Mirror only its public-image paths here; credentials
                # must never enter worker argv. One request attempt (plugins use
                # three) leaves retries to persistent backoff within the tick budget.
                image_cache = (
                    Path(cache)
                    if source in ("met", "aic", "cleveland")
                    else Path(cache) / "sources" / "images"
                )
                image = fetch_image(c.image_url, client, image_cache, attempts=1)
                pool.assess(c, image)
                pool.assess_fit(c, image, palette)
            except Exception as exc:
                log.warning(
                    "Screen-fit image unavailable for source %s; retry backed off",
                    source,
                    # Keep the sanitized error class, omitting stack source lines
                    # which can themselves contain upstream URLs or request details.
                    exc_info=(*failure_info(exc)[:2], None),
                )
            if count >= BATCH_SIZE:
                break


if __name__ == "__main__":
    import sys

    _worker(sys.argv[1], json.loads(sys.argv[2]), json.loads(sys.argv[3]))
