# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Stephen de la Vega
"""Installed, explicitly enabled and offline-entitled entry points."""

from dataclasses import dataclass
from importlib.metadata import entry_points
from importlib.resources import files
import json
import logging
from urllib.parse import urlsplit
from ventanal.extras.licence import ID, FEATURE


@dataclass(frozen=True)
class Field:
    id: str
    name: str
    kind: str = "text"
    default: str | bool = ""


@dataclass(frozen=True)
class Manifest:
    id: str
    name: str
    description: str
    kind: str
    feature: str
    privacy: str
    settings: tuple[Field, ...]
    screens: tuple[str, ...]
    renderer: object


def discover():
    result = []
    ids = set()
    screens = set()
    for ep in sorted(entry_points(group="ventanal.extras"), key=lambda e: e.name):
        try:
            m = ep.load()
            if not isinstance(m, Manifest) or not ID.fullmatch(m.id) or m.id in ids:
                raise ValueError()
            if m.kind not in ("add-on", "hosted") or not FEATURE.fullmatch(m.feature):
                raise ValueError()
            if any(
                not isinstance(v, str) or not v.strip() or len(v) > 2048
                for v in (m.name, m.description)
            ):
                raise ValueError()
            if m.kind == "hosted" and (
                not isinstance(m.privacy, str) or not m.privacy.strip()
            ):
                raise ValueError()
            if (
                not isinstance(m.privacy, str)
                or len(m.privacy) > 4096
                or not callable(m.renderer)
            ):
                raise ValueError()
            if (
                not isinstance(m.screens, tuple)
                or len(m.screens) > 2
                or len(set(m.screens)) != len(m.screens)
            ):
                raise ValueError()
            if any(
                not ID.fullmatch(s) or not s.startswith("extra-") or s in screens
                for s in m.screens
            ):
                raise ValueError()
            if len(m.settings) > 32 or len({f.id for f in m.settings}) != len(
                m.settings
            ):
                raise ValueError()
            for f in m.settings:
                if (
                    not isinstance(f, Field)
                    or not ID.fullmatch(f.id)
                    or f.id in ("csrf", "id", "enabled")
                    or not f.name
                    or f.kind not in ("text", "bool")
                ):
                    raise ValueError()
                if (
                    f.kind == "bool"
                    and type(f.default) is not bool
                    or f.kind == "text"
                    and (not isinstance(f.default, str) or len(f.default) > 1024)
                ):
                    raise ValueError()
            result.append(m)
            ids.add(m.id)
            screens.update(m.screens)
        except Exception:
            logging.getLogger(__name__).warning("Invalid extra registration ignored")
    return result


def active(installed, store, *, now=None, licences=None):
    if licences is None:
        licences = store.licences(now=now)
    features = {
        f
        for _, r in licences
        if r.entitled
        for f in r.payload["features"]
    }
    settings = store.read()["settings"]
    return [
        m
        for m in installed
        if m.feature in features and settings.get(m.id, {}).get("enabled") is True
    ]


def catalogue():
    entries = json.loads(
        files("ventanal.extras").joinpath("catalogue.json").read_text()
    )
    result = []
    if not isinstance(entries, list):
        return result
    for entry in entries:
        try:
            if not isinstance(entry, dict):
                raise ValueError()
            if entry.get("example") is True:
                continue
            if (
                not isinstance(entry.get("id"), str)
                or not ID.fullmatch(entry["id"])
                or entry.get("kind") not in ("add-on", "hosted")
                or any(
                    not isinstance(entry.get(k), str)
                    or not entry[k].strip()
                    or len(entry[k]) > 2048
                    for k in ("name", "description", "checkout_url")
                )
            ):
                raise ValueError()
            url = urlsplit(entry["checkout_url"])
            if (
                url.scheme != "https"
                or url.hostname not in ("buy.paddle.com", "checkout.paddle.com")
                or url.username
                or url.password
            ):
                raise ValueError()
            if entry["kind"] == "hosted" and (
                not isinstance(entry.get("privacy"), str)
                or not entry["privacy"].strip()
                or len(entry["privacy"]) > 4096
            ):
                raise ValueError()
            result.append(entry)
        except (ValueError, TypeError):
            logging.getLogger(__name__).warning("Invalid extras catalogue entry ignored")
    return result
