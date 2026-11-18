# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Stephen de la Vega

"""Scheduled rendering and serialized, durable publication state."""

from collections.abc import Sequence
from datetime import datetime, time, timedelta, timezone
import fcntl
import json
import logging
import os
import random
import sqlite3
from zoneinfo import ZoneInfo

import httpx

from ventanal.config import Config
from ventanal.owner import OwnerStore, SkipQueue, SPARES
from ventanal.art.pool import ArtPool, aspect_allowed, refresh_pool, season_for
from ventanal.art.sources import fetch_image
from ventanal.art.render import ART_READOUT, render_art
from ventanal.weather.data import geocode, fetch_weather, fetch_alerts
from ventanal.weather.radar import bbox_around, fetch_iem
from ventanal.weather.basemap import basemap
from ventanal.weather.render import WEATHER_READOUT, render_weather
from ventanal.quantize import quantize, simulate
from ventanal.frame import encode_frame
from ventanal.manifest import publish, read_manifest, write_manifest

log = logging.getLogger(__name__)
TICK_ERROR = "Frame publication or pool refresh failed"


def current_slot(
    now: datetime, slots: Sequence[time], lead_minutes: int = 5
) -> datetime:
    """Return the latest local slot at or before the lead-adjusted time."""
    if now.utcoffset() is None:
        raise ValueError("now must be timezone-aware")
    if not slots:
        raise ValueError("slots must not be empty")
    cutoff = now + timedelta(minutes=lead_minutes)
    candidates = [
        datetime.combine(cutoff.date() - timedelta(days=days), slot, now.tzinfo)
        for days in (0, 1)
        for slot in slots
    ]
    return max(slot for slot in candidates if slot <= cutoff)


def fetch_candidate(candidate, client, cfg):
    """Dispatch plugin-specific fetches; preserve the original museum cache API."""
    from ventanal.art.image_cache import configure
    configure(cfg)
    if candidate.source in ("met", "aic", "cleveland"):
        return fetch_image(candidate.image_url, client, cfg.cache_dir)
    from ventanal.art import sources

    source = sources.registry(cache_dir=cfg.cache_dir, credentials=cfg.credentials).get(
        candidate.source
    )
    if source is None:
        raise ValueError("Artwork source is unavailable")
    return source.fetch_image(candidate, client)


class ArtworkAspectError(ValueError):
    """A catalogue entry or its decoded artwork exceeds the aspect limit."""


def fetch_artwork(candidate, client, cfg):
    """Check both catalogue metadata and the actual image before composing."""
    if not aspect_allowed(candidate.width, candidate.height):
        raise ArtworkAspectError("artwork metadata exceeds 3:1")
    image = fetch_candidate(candidate, client, cfg)
    if not aspect_allowed(*image.size):
        raise ArtworkAspectError("decoded artwork exceeds 3:1")
    return image


def select_artwork(cfg, now, client, pool, *, excluded=(), strict=False):
    """Try candidates until decoded dimensions and colour pass."""
    excluded = set(excluded)
    while True:
        candidate = pool.pick_next(
            season_for(now.date(), cfg.seasons).name,
            now,
            cfg.no_repeat_days,
            random.Random(),
            excluded=excluded,
            museums=cfg.museums,
            strict=strict,
            include_monotone=cfg.include_monotone,
            screen_fit_only=cfg.screen_fit_only,
            panel_rgb=cfg.panel_rgb,
        )
        if candidate is None:
            raise ValueError("no eligible artwork")
        key = f"{candidate.source}:{candidate.object_id}"
        try:
            image = fetch_artwork(candidate, client, cfg)
        except ArtworkAspectError:
            excluded.add(key)
            continue
        except Exception as exc:
            from ventanal.art.sources import failure_info

            log.warning(
                "Artwork image unavailable for source %s",
                candidate.source,
                exc_info=failure_info(exc),
            )
            excluded.add(key)
            continue
        colourful = pool.assess(candidate, image)
        if cfg.include_monotone or colourful:
            return candidate, image
        excluded.add(key)


def keep_fallback(cfg, pool, current, replacement):
    """Do not churn a valid reserved fallback for a worse-scoring candidate."""
    if (
        current is None
        or not aspect_allowed(current.width, current.height)
        or current.source not in cfg.museums
        or f"{current.source}:{current.object_id}" in cfg.ignored
    ):
        return False
    if not cfg.include_monotone and not pool.colour_status(current):
        return False
    old = pool.fit_score(current, cfg.panel_rgb)
    new = pool.fit_score(replacement, cfg.panel_rgb)
    return old is not None and (new is None or new <= old)


def art_canvas(cfg, now, client, pool):
    excluded = set(cfg.ignored)
    excluded.update(
        f"{source}:{object_id}"
        for _, _, source, object_id, _ in OwnerStore(cfg.cache_dir).spares()
    )
    candidate, image = select_artwork(cfg, now, client, pool, excluded=excluded)
    canvas, credit = render_art(image, candidate)
    return canvas, credit, candidate


def weather_canvas(cfg, now, client):
    lat, lon, place = geocode(cfg, client, cfg.cache_dir)
    weather = fetch_weather(lat, lon, cfg.units, client, now=now)
    try:
        alerts = fetch_alerts(lat, lon, client, cfg.nws_contact)
    except Exception as exc:
        log.warning("alerts unavailable: %s", exc)
        alerts = []
    size = (782, 462)
    bbox = bbox_around(lat, lon, cfg.radar_radius_miles, size[0] / size[1])
    radar = None
    if cfg.radar == "iem":
        try:
            radar = fetch_iem(bbox, size, client, cfg.cache_dir)
        except Exception as exc:
            log.warning("radar unavailable: %s", exc)
    try:
        lines = basemap(bbox, size, cfg.cache_dir, client)
    except Exception as exc:
        log.warning("basemap unavailable: %s", exc)
        lines = {"states": [], "counties": []}
    return render_weather(weather, alerts, radar, lines, place, (lat, lon), now, cfg)


def _publish(cfg, screen, canvas, credit, now, store):
    indices = quantize(canvas, cfg.panel_rgb)
    entry = publish(
        cfg.output_dir,
        screen,
        encode_frame(indices),
        WEATHER_READOUT if screen == "weather" else ART_READOUT,
        credit,
        simulate(indices, cfg.panel_rgb),
        now,
    )
    store.role(entry["id"], screen)
    store.event("publish", f'{screen} {entry["id"]}')
    log.info("published %s id=%s", screen, entry["id"])
    return entry


def _elapsed(now, then):
    return now.astimezone(timezone.utc) - then.astimezone(timezone.utc)


def ensure_pool(cfg: Config, pool: ArtPool, client: httpx.Client, now: datetime) -> int:
    """Refresh stale pool contents and record cache freshness for all callers."""
    refreshed = pool.last_refresh()
    if refreshed is None or _elapsed(now, refreshed) > timedelta(hours=24):
        retry_after = pool.retry_after()
        if retry_after is not None and _elapsed(now, retry_after) < timedelta(0):
            return 0
        return refresh_pool(cfg, pool, client, now.date(), now=now)
    return 0


def tick(
    cfg: Config,
    now: datetime,
    client: httpx.Client,
    *,
    force_art: bool = False,
    force_spares: bool = False,
    force_weather: bool = False,
) -> dict:
    """Publish due screens independently; failed screens remain due for retry."""
    if now.utcoffset() is None:
        raise ValueError("now must be timezone-aware")
    now = now.astimezone(ZoneInfo(cfg.timezone))
    cfg.cache_dir.mkdir(parents=True, exist_ok=True)
    # Download/import before acquiring the manifest lock: weather/art can proceed
    # while another process owns the FCC lock or this tick imports the weekly DB.
    if cfg.ham_call_sign:
        from ventanal.solar.licence import refresh, RefreshRunning
        try:
            refresh(cfg.cache_dir,client,now)
        except RefreshRunning:
            log.info('FCC database refresh already running')
        except Exception:
            log.warning('FCC database refresh failed; previous index retained')
    # The timer and manual invocations share the manifest's single-writer lock.
    with (cfg.cache_dir / "tick.lock").open("a") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        return _tick_locked(cfg, now, client, force_art, force_spares, force_weather)


def _tick_locked(cfg, now, client, force_art, force_spares, force_weather):
    from ventanal.status import BoardStore

    from ventanal.art.image_cache import configure, prune
    configure(cfg)
    store = BoardStore(cfg.cache_dir)
    # Recover and record before solar/extras can replace the damaged manifest.
    recovered = read_manifest(cfg.output_dir, store=store)
    write_manifest(cfg.output_dir, recovered)
    prune(cfg.cache_dir)
    from ventanal.solar.publish import tick_solar
    tick_solar(cfg, now, client)
    from ventanal.extras.publish import tick_extras
    tick_extras(cfg, now)
    manifest = read_manifest(cfg.output_dir)
    manifest["quiet"] = {"start": cfg.quiet_start, "end": cfg.quiet_end}
    write_manifest(cfg.output_dir, manifest)
    result = {"art": False, "weather": False, "pool": 0}
    path = cfg.cache_dir / "state.json"
    try:
        state = json.loads(path.read_text())
        if not isinstance(state, dict):
            raise ValueError("expected a JSON object")
    except FileNotFoundError:
        state = {}
    except (OSError, ValueError) as exc:
        log.warning("state.json unreadable; treating as empty: %s", exc)
        state = {}

    def save():
        temporary = path.with_suffix(".json.tmp")
        temporary.write_text(json.dumps(state, indent=2) + "\n")
        os.replace(temporary, path)

    pool = ArtPool(cfg.cache_dir / "art.sqlite3")
    owner = OwnerStore(cfg.cache_dir)
    # Backfill identity for a manifest created by older server versions.
    old_art = read_manifest(cfg.output_dir)["screens"].get("art", {})
    old_credit = old_art.get("credit") or {}
    if "source" in old_credit and "object_id" in old_credit:
        candidate = pool.get(old_credit["source"], old_credit["object_id"])
        if candidate is not None:
            owner.register(old_art["id"], candidate)
    try:
        SkipQueue(cfg.cache_dir).drain(owner)
    except sqlite3.Error:
        log.warning("Unable to process queued skips; pending reports will retry")
    try:
        result["pool"] = ensure_pool(cfg, pool, client, now)
    except Exception as exc:
        log.warning("pool refresh failed: %s", exc)
    from ventanal.art.screen_fit import batch

    try:
        batch(cfg)
    except Exception as exc:
        from ventanal.art.sources import failure_info

        log.warning(
            "Screen-fit batch unavailable", exc_info=(*failure_info(exc)[:2], None)
        )
    slot = current_slot(now, cfg.art_slots).isoformat()
    if force_art:
        # Reuse the manifest identity, including installations predating this flag.
        # Do not choose artwork, advance the slot, or extend no-repeat history.
        try:
            from ventanal.calibration_ui import current_painting

            credit = current_painting(cfg)["credit"]
            candidate = pool.get(credit["source"], credit["object_id"])
            if candidate is None:
                raise ValueError("published artwork is absent from the pool")
            try:
                image = fetch_artwork(candidate, client, cfg)
            except ArtworkAspectError:
                image = None
            if (
                image is not None
                and not cfg.include_monotone
                and not pool.assess(candidate, image)
            ):
                raise ValueError(
                    "artwork is monotone under the current selection policy"
                )
            from ventanal.art.screen_fit import THRESHOLD

            score = pool.fit_score(candidate, cfg.panel_rgb)
            if image is None or (
                cfg.screen_fit_only
                and (score is None or score < THRESHOLD
                     or f"{candidate.source}:{candidate.object_id}" in cfg.ignored)
            ):
                canvas, credit, candidate = art_canvas(cfg, now, client, pool)
            else:
                canvas, credit = render_art(image, candidate)
            entry = _publish(cfg, "art", canvas, credit, now, store)
            owner.register(entry["id"], candidate)
            result["art"] = True
        except Exception as exc:
            log.error("forced art publish failed: %s", exc)
            store.event("tick error", TICK_ERROR)
    elif state.get("last_art_slot") != slot:
        try:
            canvas, credit, candidate = art_canvas(cfg, now, client, pool)
            entry = _publish(cfg, "art", canvas, credit, now, store)
            owner.register(entry["id"], candidate)
            pool.mark_shown(candidate, now)
            state["last_art_slot"] = slot
            save()
            result["art"] = True
        except Exception as exc:
            log.error("art publish failed: %s", exc)
            store.event("tick error", TICK_ERROR)
    last_weather = state.get("last_weather")
    if (
        force_weather
        or last_weather is None
        or _elapsed(now, datetime.fromisoformat(last_weather))
        >= timedelta(minutes=cfg.weather_interval_minutes - 5)
    ):
        try:
            canvas = weather_canvas(cfg, now, client)
            _publish(cfg, "weather", canvas, None, now, store)
            state["last_weather"] = now.isoformat()
            save()
            result["weather"] = True
        except Exception as exc:
            log.error("weather publish failed: %s", exc)
            store.event("tick error", TICK_ERROR)
    # Slots survive main-art rotation and restarts; only reported slots refill.
    existing = {row[0]: row for row in owner.spares()}
    if force_spares:
        result["spares"] = True
    for index in range(SPARES if force_spares or not force_art else 0):
        if force_spares:
            # Re-render reserved identities, including pending replacement slots.
            # Retain their replacement flag and do not alter shown history.
            if index not in existing:
                continue
            _, _, source, object_id, needs_replacement = existing[index]
            try:
                candidate = pool.get(source, object_id)
                if candidate is None:
                    raise ValueError("reserved artwork is absent from the pool")
                try:
                    image = fetch_artwork(candidate, client, cfg)
                except ArtworkAspectError:
                    image = None
                if (
                    image is not None
                    and not cfg.include_monotone
                    and not pool.assess(candidate, image)
                ):
                    raise ValueError(
                        "spare is monotone under the current selection policy"
                    )
                from ventanal.art.screen_fit import THRESHOLD

                score = pool.fit_score(candidate, cfg.panel_rgb)
                if image is None or (
                    cfg.screen_fit_only
                    and (score is None or score < THRESHOLD
                         or f"{candidate.source}:{candidate.object_id}" in cfg.ignored)
                ):
                    excluded = set(cfg.ignored) | {
                        f"{s}:{o}" for _, _, s, o, _ in owner.spares()
                    }
                    main = (
                        read_manifest(cfg.output_dir)["screens"]
                        .get("art", {})
                        .get("credit")
                        or {}
                    )
                    if "source" in main:
                        excluded.add(f"{main['source']}:{main['object_id']}")
                    replacement, replacement_image = select_artwork(
                        cfg, now, client, pool, excluded=excluded, strict=True
                    )
                    if image is None or not keep_fallback(cfg, pool, candidate, replacement):
                        candidate, image = replacement, replacement_image
                canvas, credit = render_art(image, candidate)
                entry = _publish(cfg, f"spare{index}", canvas, credit, now, store)
                owner.put_spare(index, entry, candidate, replace_due=needs_replacement)
            except Exception as exc:
                result["spares"] = False
                log.error("forced spare slot=%d publish failed: %s", index, exc)
                store.event("tick error", TICK_ERROR)
            continue
        reserved = None
        if index in existing and not existing[index][4]:
            reserved = pool.get(existing[index][2], existing[index][3])
            if reserved is not None and aspect_allowed(reserved.width, reserved.height):
                if cfg.include_monotone and not cfg.screen_fit_only:
                    continue
                try:
                    colourful = pool.colour_status(reserved)
                    if colourful is None:
                        colourful = pool.assess(
                            reserved, fetch_artwork(reserved, client, cfg)
                        )
                    from ventanal.art.screen_fit import THRESHOLD

                    score = pool.fit_score(reserved, cfg.panel_rgb)
                    if (cfg.include_monotone or colourful) and (
                        not cfg.screen_fit_only
                        or (score is not None and score >= THRESHOLD)
                    ):
                        continue
                except ArtworkAspectError:
                    reserved = None
                except Exception as exc:
                    from ventanal.art.sources import failure_info

                    log.warning(
                        "Reserved spare assessment deferred", exc_info=failure_info(exc)
                    )
                    continue
        try:
            excluded = set(cfg.ignored)
            excluded.update(f"{s}:{o}" for _, _, s, o, _ in owner.spares())
            main = (
                read_manifest(cfg.output_dir)["screens"].get("art", {}).get("credit")
                or {}
            )
            if "source" in main:
                excluded.add(f"{main['source']}:{main['object_id']}")
            candidate, image = select_artwork(
                cfg, now, client, pool, excluded=excluded, strict=True
            )
            if cfg.screen_fit_only and keep_fallback(cfg, pool, reserved, candidate):
                continue
            canvas, credit = render_art(image, candidate)
            entry = _publish(cfg, f"spare{index}", canvas, credit, now, store)
            owner.put_spare(index, entry, candidate)
        except Exception as exc:
            log.warning("spare slot=%d deferred: %s", index, exc)
            break
    manifest = read_manifest(cfg.output_dir)
    manifest["spares"] = [entry for _, entry, _, _, _ in owner.spares()]
    write_manifest(cfg.output_dir, manifest)
    return result
