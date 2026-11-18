# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Stephen de la Vega

"""Load and validate Ventanal's TOML configuration without side effects."""

from dataclasses import dataclass, field
from datetime import date, time
from pathlib import Path
import fcntl
import re
import tomllib
from typing import Literal
from zoneinfo import ZoneInfo, ZoneInfoNotFoundError


def normalize_hostname(value: str) -> str | None:
    name = value.removesuffix(".").lower()
    label = r"[a-z0-9](?:[a-z0-9-]{0,61}[a-z0-9])?"
    if value.isascii() and len(name) <= 253 and re.fullmatch(rf"{label}(?:\.{label})*", name):
        return name
    return None


@dataclass(frozen=True)
class Season:
    """Inclusive month/day bounds; start greater than end wraps the year."""

    name: str
    start: tuple[int, int]
    end: tuple[int, int]
    terms: tuple[str, ...]


@dataclass(frozen=True)
class Config:
    zip: str | None
    latitude: float | None
    longitude: float | None
    place_name: str | None
    timezone: str
    units: Literal["imperial", "metric"]
    clock_24h: bool
    weather_interval_minutes: int
    art_slots: tuple[time, ...]
    museums: tuple[str, ...]
    rijksmuseum_key: str = field(repr=False)
    no_repeat_days: int
    seasons: tuple[Season, ...]
    radar: Literal["iem", "none"]
    here_label: Literal["off", "en", "es"]
    radar_radius_miles: int
    nws_contact: str
    panel_rgb: tuple[tuple[int, int, int], ...]
    listen_host: str
    listen_port: int
    output_dir: Path
    cache_dir: Path
    image_cache_bytes: int = 2 * 1024**3
    https_host: str = "0.0.0.0"
    https_port: int = 8766
    allowed_hosts: tuple[str, ...] = ()
    ham_call_sign: str = ""
    battery_full_mv: int | None = None
    ignored: tuple[str, ...] = ()
    quiet_start: str = "23:00"
    quiet_end: str = "06:00"
    include_monotone: bool = False
    screen_fit_only: bool = False
    credentials: dict[str, dict[str, str]] = field(default_factory=dict, repr=False)


class ConfigError(ValueError):
    """A configuration file could not be read or contains an invalid value."""


def _string(value, key: str, *, empty: bool = False) -> str:
    if not isinstance(value, str) or (not empty and not value.strip()):
        raise ConfigError(f"{key}: expected {'a string' if empty else 'a nonempty string'}")
    return value


def _integer(value, key: str, minimum: int) -> int:
    if type(value) is not int or value < minimum:
        raise ConfigError(f"{key}: expected an integer >= {minimum}")
    return value


def _strings(value, key: str) -> tuple[str, ...]:
    if not isinstance(value, list) or not value:
        raise ConfigError(f"{key}: expected a nonempty array of strings")
    return tuple(_string(item, key) for item in value)


def _choice(value, key: str, choices: tuple[str, ...]) -> str:
    if value not in choices:
        raise ConfigError(f"{key}: expected one of {', '.join(choices)}")
    return value


def _month_day(value, key: str) -> tuple[int, int]:
    if not isinstance(value, str) or not re.fullmatch(r"[0-9]{2}-[0-9]{2}", value):
        raise ConfigError(f"{key}: expected MM-DD")
    month, day = map(int, value.split("-"))
    try:
        date(2000, month, day)
    except ValueError as exc:
        raise ConfigError(f"{key}: invalid calendar date") from exc
    return month, day


def _clock(value, key):
    if not isinstance(value, str) or not re.fullmatch(r"[0-9]{2}:[0-9]{2}", value):
        raise ConfigError(f"{key}: expected HH:MM")
    try:
        time.fromisoformat(value)
    except ValueError as exc:
        raise ConfigError(f"{key}: invalid time") from exc
    return value


def load_config(path: Path) -> Config:
    """Read a TOML file; report invalid settings with their dotted key names.

    Location may use a US ZIP (preserving ZIP+4) or a latitude/longitude pair.
    Other settings are explicit, as shown in ventanal.example.toml.
    """
    try:
        with path.open("rb") as source:
            fcntl.flock(source, fcntl.LOCK_SH)
            data = tomllib.load(source)
    except (OSError, ValueError) as exc:
        raise ConfigError("config.file: unable to read valid TOML") from exc

    return validate_config(data)


def validate_config(data: dict) -> Config:
    """Shared validation for files and browser edits, without filesystem effects."""
    sections = {}
    for name in ("location", "display", "schedule", "art", "weather", "panel", "server"):
        section = data.get(name)
        if not isinstance(section, dict):
            raise ConfigError(f"{name}.section: expected a table")
        sections[name] = section
    location, display, schedule, art, weather, panel, server = sections.values()

    zip_code = location.get("zip")
    if zip_code is not None:
        if not isinstance(zip_code, str) or not re.fullmatch(r"[0-9]{5}(?:-[0-9]{4})?", zip_code):
            raise ConfigError("location.zip: expected a five-digit US ZIP or ZIP+4")
    coordinates = []
    for key, limit in (("latitude", 90), ("longitude", 180)):
        value = location.get(key)
        if value is not None:
            if type(value) not in (int, float) or not -limit <= value <= limit:
                raise ConfigError(f"location.{key}: expected a number between {-limit} and {limit}")
            value = float(value)
        coordinates.append(value)
    latitude, longitude = coordinates
    if latitude is not None and longitude is None:
        raise ConfigError("location.longitude: required with latitude")
    if longitude is not None and latitude is None:
        raise ConfigError("location.latitude: required with longitude")
    if zip_code is None and latitude is None:
        raise ConfigError("location.zip: provide a ZIP or latitude/longitude")
    place_name = location.get("place_name")
    if place_name is not None:
        place_name = _string(place_name, "location.place_name")
    timezone = _string(location.get("timezone"), "location.timezone")
    try:
        ZoneInfo(timezone)
    except (ZoneInfoNotFoundError, ValueError) as exc:
        raise ConfigError("location.timezone: expected an IANA timezone") from exc

    slots = []
    for value in _strings(schedule.get("art_slots"), "schedule.art_slots"):
        if not re.fullmatch(r"[0-9]{2}:[0-9]{2}", value):
            raise ConfigError("schedule.art_slots: expected HH:MM times")
        try:
            slots.append(time.fromisoformat(value))
        except ValueError as exc:
            raise ConfigError("schedule.art_slots: invalid time") from exc

    ignored = art.get("ignored", [])
    if not isinstance(ignored, list) or any(
        not isinstance(item, str) or not re.fullmatch(r"[a-z][a-z0-9_-]*:[A-Za-z0-9_-]+", item)
        for item in ignored
    ):
        raise ConfigError("art.ignored: expected source:object_id strings")

    seasons = []
    raw_seasons = art.get("seasons")
    if not isinstance(raw_seasons, list) or not raw_seasons:
        raise ConfigError("art.seasons: expected a nonempty array of tables")
    for index, entry in enumerate(raw_seasons):
        key = f"art.seasons[{index}]"
        if not isinstance(entry, dict):
            raise ConfigError(f"{key}: expected a table")
        seasons.append(Season(
            name=_string(entry.get("name"), f"{key}.name"),
            start=_month_day(entry.get("start"), f"{key}.start"),
            end=_month_day(entry.get("end"), f"{key}.end"),
            terms=_strings(entry.get("terms"), f"{key}.terms"),
        ))

    palette = []
    for colour in ("black", "white", "yellow", "red", "blue", "green"):
        value = panel.get(colour)
        if not isinstance(value, str) or not re.fullmatch(r"#[0-9a-fA-F]{6}", value):
            raise ConfigError(f"panel.{colour}: expected #RRGGBB")
        palette.append(tuple(int(value[offset:offset + 2], 16) for offset in (1, 3, 5)))

    listen = _string(server.get("listen"), "server.listen")
    host, separator, port = listen.rpartition(":")
    if not separator or not host.strip() or not re.fullmatch(r"[0-9]+", port) or not 1 <= int(port) <= 65535:
        raise ConfigError("server.listen: expected host:port with port 1..65535")
    if host.startswith("[") and host.endswith("]"):
        host = host[1:-1]

    secure = _string(server.get("https_listen", "0.0.0.0:8766"), "server.https_listen")
    shost, sep, sport = secure.rpartition(":")
    if not sep or not shost.strip() or not sport.isdecimal() or not 1 <= int(sport) <= 65535:
        raise ConfigError("server.https_listen: expected host:port with port 1..65535")
    if shost.startswith("[") and shost.endswith("]"): shost = shost[1:-1]

    allowed_hosts = server.get("allowed_hosts", [])
    if not isinstance(allowed_hosts, list) or any(
        not isinstance(name, str) or normalize_hostname(name) is None for name in allowed_hosts
    ):
        raise ConfigError("server.allowed_hosts: expected an array of hostnames without ports")

    credentials = art.get('credentials', {})
    if not isinstance(credentials, dict) or any(
        not isinstance(ident, str) or not isinstance(fields, dict) or any(
            not isinstance(key, str) or not isinstance(value, str) for key, value in fields.items()
        ) for ident, fields in credentials.items()
    ):
        raise ConfigError('art.credentials: expected source tables of string credentials')

    if type(art.get('screen_fit_only', False)) is not bool:
        raise ConfigError('art.screen_fit_only: expected a boolean')
    if type(art.get('include_monotone', False)) is not bool:
        raise ConfigError('art.include_monotone: expected a boolean')

    battery = data.get("battery", {})
    if not isinstance(battery, dict):
        raise ConfigError("battery.section: expected a table")
    full_mv = battery.get("full_mv")
    if full_mv is not None and (type(full_mv) is not int or not 4000 <= full_mv <= 4300):
        raise ConfigError("battery.full_mv: expected 4000–4300 mV")

    from ventanal.solar.licence import normalize_call
    ham = data.get("ham", {})
    if not isinstance(ham, dict) or not isinstance(ham.get("call_sign", ""), str):
        raise ConfigError("ham.call_sign: expected a US amateur call sign")
    try: call_sign = normalize_call(ham.get("call_sign", ""))
    except ValueError as exc: raise ConfigError("ham.call_sign: " + str(exc)) from None

    return Config(
        image_cache_bytes=_integer(art.get("image_cache_bytes", 2 * 1024**3), "art.image_cache_bytes", 1),
        ham_call_sign=call_sign,
        battery_full_mv=full_mv,
        include_monotone=art.get('include_monotone', False),
        screen_fit_only=art.get('screen_fit_only', False),
        credentials=credentials,
        https_host=shost, https_port=int(sport),
        quiet_start=_clock(schedule.get("quiet_start", "23:00"), "schedule.quiet_start"),
        quiet_end=_clock(schedule.get("quiet_end", "06:00"), "schedule.quiet_end"),
        ignored=tuple(ignored),
        zip=zip_code,
        latitude=latitude,
        longitude=longitude,
        place_name=place_name,
        timezone=timezone,
        units=_choice(display.get("units"), "display.units", ("imperial", "metric")),
        clock_24h=_choice(display.get("clock"), "display.clock", ("12h", "24h")) == "24h",
        weather_interval_minutes=_integer(schedule.get("weather_interval_minutes"), "schedule.weather_interval_minutes", 1),
        art_slots=tuple(slots),
        museums=_strings(art.get("museums"), "art.museums"),
        rijksmuseum_key=_string(art.get("rijksmuseum_key", ""), "art.rijksmuseum_key", empty=True),
        no_repeat_days=_integer(art.get("no_repeat_days"), "art.no_repeat_days", 0),
        seasons=tuple(seasons),
        radar=_choice(weather.get("radar"), "weather.radar", ("iem", "none")),
        here_label=_choice(weather.get("here_label", "off"), "weather.here_label", ("off", "en", "es")),
        radar_radius_miles=_integer(weather.get("radar_radius_miles"), "weather.radar_radius_miles", 1),
        nws_contact=_string(weather.get("nws_contact"), "weather.nws_contact", empty=True),
        panel_rgb=tuple(palette),
        listen_host=host,
        listen_port=int(port),
        allowed_hosts=tuple(normalize_hostname(name) for name in allowed_hosts),
        output_dir=Path(_string(server.get("output_dir"), "server.output_dir")),
        cache_dir=Path(_string(server.get("cache_dir"), "server.cache_dir")),
    )
