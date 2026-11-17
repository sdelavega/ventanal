# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Stephen de la Vega
"""Publish at most two optional screens under the existing tick lock."""

import logging
from ventanal.extras import registry
from ventanal.extras.licence import Store
from ventanal.manifest import Readout, publish, read_manifest, write_manifest
from ventanal.frame import decode_frame


def filter_manifest(manifest, directory, *, now=None):
    allowed = [
        s
        for m in registry.active(registry.discover(), Store(directory), now=now)
        for s in m.screens
    ][:2]
    screens = manifest.setdefault("screens", {})
    for name in list(screens):
        if name not in ("art", "weather", "solar") and name not in allowed:
            screens.pop(name)
    order = [s for s in ("art", "weather", "solar", *allowed) if s in screens]
    manifest["screens"] = {s: screens[s] for s in order}
    manifest["screen_order"] = order
    return manifest


def tick_extras(cfg, now):
    store = Store(cfg.cache_dir)
    settings = store.read()["settings"]
    remaining = 2
    installed = registry.active(registry.discover(), store, now=now.timestamp())
    current = read_manifest(cfg.output_dir)
    if not installed and not any(
        s.startswith("extra-") for s in current.get("screens", {})
    ):
        return
    for m in installed:
        permitted = m.screens[:remaining]
        remaining -= len(permitted)
        if not permitted:
            continue
        try:
            values = {
                f.id: settings.get(m.id, {}).get("values", {}).get(f.id, f.default)
                for f in m.settings
            }
            rendered = m.renderer(cfg, now, values)
            if not isinstance(rendered, dict) or set(rendered) - set(m.screens):
                raise ValueError()
            for name in permitted:
                if name not in rendered:
                    continue
                data, readout, preview = rendered[name]
                if (
                    not isinstance(readout, Readout)
                    or any(
                        type(v) is not int
                        for v in (readout.x, readout.y, readout.w, readout.h)
                    )
                    or readout.style not in ("lcars", "shade")
                    or not (
                        0 <= readout.x < 1600
                        and 0 <= readout.y < 1200
                        and 0 < readout.w <= 1600 - readout.x
                        and 0 < readout.h <= 1200 - readout.y
                    )
                ):
                    raise ValueError("Invalid extra readout")
                decode_frame(
                    data
                )  # Reject malformed/unbounded frames before publication.
                publish(cfg.output_dir, name, data, readout, None, preview, now)
        except Exception:
            logging.getLogger(__name__).warning(
                "Extra publication failed; previous frame retained"
            )
    write_manifest(
        cfg.output_dir,
        filter_manifest(
            read_manifest(cfg.output_dir), cfg.cache_dir, now=now.timestamp()
        ),
    )
