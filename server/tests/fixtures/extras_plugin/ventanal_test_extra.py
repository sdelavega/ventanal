# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Stephen de la Vega
"""Installed only by tests through a temporary sys.path entry."""

import numpy as np
from PIL import Image
from ventanal.extras.registry import Manifest, Field
from ventanal.frame import encode_frame
from ventanal.manifest import Readout


def render(cfg, now, settings):
    return {
        "extra-test": (
            encode_frame(np.zeros((1200, 1600), dtype=np.uint8)),
            Readout(10, 10, 300, 100, "lcars"),
            Image.new("RGB", (800, 600), "white"),
        )
    }


manifest = Manifest(
    "test-extra",
    "Test extra",
    "Example screen used only by tests",
    "add-on",
    "test.feature",
    "",
    (Field("caption", "Caption", "text", "Example"),),
    ("extra-test",),
    render,
)
