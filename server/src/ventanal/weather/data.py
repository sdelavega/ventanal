# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Stephen de la Vega

"""Fetch weather using caller-owned HTTP clients; no network at import time."""

from dataclasses import dataclass
from datetime import date, datetime, timezone
import json
import os
from pathlib import Path
from tempfile import NamedTemporaryFile
from zoneinfo import ZoneInfo

from ventanal.config import Config


from ventanal import USER_AGENT

WMO_LONG: dict[int, str] = {
    0: 'Clear sky', 1: 'Mainly clear', 2: 'Partly cloudy', 3: 'Overcast',
    45: 'Fog', 48: 'Depositing rime fog',
    51: 'Light drizzle', 53: 'Moderate drizzle', 55: 'Dense drizzle',
    56: 'Light freezing drizzle', 57: 'Dense freezing drizzle',
    61: 'Slight rain', 63: 'Moderate rain', 65: 'Heavy rain',
    66: 'Light freezing rain', 67: 'Heavy freezing rain',
    71: 'Slight snow fall', 73: 'Moderate snow fall', 75: 'Heavy snow fall',
    77: 'Snow grains', 80: 'Slight rain showers', 81: 'Moderate rain showers',
    82: 'Violent rain showers', 85: 'Slight snow showers', 86: 'Heavy snow showers',
    95: 'Thunderstorm', 96: 'Thunderstorm with slight hail',
    99: 'Thunderstorm with heavy hail',
}
WMO_SHORT: dict[int, str] = {
    0: 'CLEAR', 1: 'M CLEAR', 2: 'P CLOUD', 3: 'OVERCAST',
    45: 'FOG', 48: 'RIME FOG', 51: 'DRIZZLE', 53: 'DRIZZLE', 55: 'DRIZZLE',
    56: 'FRZ DRZ', 57: 'FRZ DRZ', 61: 'RAIN', 63: 'RAIN', 65: 'HVY RAIN',
    66: 'FRZ RAIN', 67: 'FRZ RAIN', 71: 'SNOW', 73: 'SNOW', 75: 'HVY SNOW',
    77: 'SN GRAIN', 80: 'SHOWERS', 81: 'SHOWERS', 82: 'HVY SHWR',
    85: 'SN SHWR', 86: 'SN SHWR', 95: 'T-STORM', 96: 'T / HAIL', 99: 'T / HAIL',
}


@dataclass
class Current:
    temp: float
    humidity: int
    wind_speed: float
    wind_dir: str
    code: int


@dataclass
class Day:
    date: date
    hi: int
    lo: int
    code: int
    precip_prob: int


@dataclass
class Alert:
    event: str
    headline: str
    severity: str
    ends: datetime | None


@dataclass
class Weather:
    current: Current
    days: list[Day]
    sunrise: datetime
    sunset: datetime
    rain_onset: datetime | None
    fetched: datetime


def compass8(deg: float) -> str:
    return ('N', 'NE', 'E', 'SE', 'S', 'SW', 'W', 'NW')[int((deg % 360 + 22.5) // 45) % 8]


def geocode(cfg: Config, client, cache_dir) -> tuple[float, float, str]:
    """Prefer explicit coordinates; cache ZIP results keyed by five-digit ZIP."""
    if cfg.latitude is not None and cfg.longitude is not None:
        return cfg.latitude, cfg.longitude, cfg.place_name or f'{cfg.latitude:.4f}, {cfg.longitude:.4f}'
    if cfg.zip is None:
        raise ValueError('A ZIP or latitude/longitude pair is required')
    zip_code = cfg.zip[:5]
    path = Path(cache_dir) / 'geocode.json'
    try:
        cached = json.loads(path.read_text())
        if cached['zip'] == zip_code:
            return float(cached['latitude']), float(cached['longitude']), cfg.place_name or cached['name']
    except (OSError, ValueError, KeyError, TypeError):
        pass

    response = client.get(f'https://api.zippopotam.us/us/{zip_code}', headers={'User-Agent': USER_AGENT})
    response.raise_for_status()
    place = response.json()['places'][0]
    lat, lon = float(place['latitude']), float(place['longitude'])
    name = f"{place['place name']} · {place['state abbreviation']}".upper()
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = None
    try:
        with NamedTemporaryFile(mode='w', dir=path.parent, delete=False) as stream:
            temporary = Path(stream.name)
            json.dump({'zip': zip_code, 'latitude': lat, 'longitude': lon, 'name': name}, stream)
        os.replace(temporary, path)
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)
    return lat, lon, cfg.place_name or name


def fetch_weather(lat, lon, units, client, *, now: datetime | None = None) -> Weather:
    """Parse local forecast times in the API's timezone; rain must be after now."""
    instant = now if now is not None else datetime.now(timezone.utc)
    if instant.utcoffset() is None:
        raise ValueError('now must be timezone-aware')
    params = {
        'latitude': lat, 'longitude': lon,
        'current': 'temperature_2m,relative_humidity_2m,weather_code,wind_speed_10m,wind_direction_10m',
        'hourly': 'precipitation_probability',
        'daily': 'weather_code,temperature_2m_max,temperature_2m_min,precipitation_probability_max,sunrise,sunset',
        'timezone': 'auto', 'forecast_days': 7,
    }
    if units == 'imperial':
        params.update(temperature_unit='fahrenheit', wind_speed_unit='mph')
    response = client.get('https://api.open-meteo.com/v1/forecast', params=params, headers={'User-Agent': USER_AGENT})
    response.raise_for_status()
    data = response.json()
    zone = ZoneInfo(data['timezone'])

    def local_time(value: str) -> datetime:
        parsed = datetime.fromisoformat(value)
        return parsed.replace(tzinfo=zone) if parsed.tzinfo is None else parsed.astimezone(zone)

    current = data['current']
    daily = data['daily']
    days = [Day(
        date=date.fromisoformat(value), hi=round(daily['temperature_2m_max'][i]),
        lo=round(daily['temperature_2m_min'][i]), code=int(daily['weather_code'][i]),
        precip_prob=int(daily['precipitation_probability_max'][i]),
    ) for i, value in enumerate(daily['time'])]
    today = instant.astimezone(zone).date()
    rain_hours = [local_time(value) for value, probability in zip(
        data['hourly']['time'], data['hourly']['precipitation_probability'], strict=True,
    ) if probability is not None and probability >= 40]
    rain_onset = min((hour for hour in rain_hours if hour.date() == today and hour > instant), default=None)
    return Weather(
        current=Current(float(current['temperature_2m']), int(current['relative_humidity_2m']),
                        float(current['wind_speed_10m']), compass8(current['wind_direction_10m']),
                        int(current['weather_code'])),
        days=days, sunrise=local_time(daily['sunrise'][0]), sunset=local_time(daily['sunset'][0]),
        rain_onset=rain_onset, fetched=instant,
    )


def fetch_alerts(lat, lon, client, contact: str) -> list[Alert]:
    """Return active NWS alerts in descending severity, preserving ties."""
    response = client.get(
        'https://api.weather.gov/alerts/active', params={'point': f'{lat:.4f},{lon:.4f}'},
        headers={'User-Agent': USER_AGENT + (f' <{contact}>' if contact else ''),
                 'Accept': 'application/geo+json'},
    )
    response.raise_for_status()
    alerts = []
    for feature in response.json()['features']:
        props = feature['properties']
        alerts.append(Alert(
            event=props['event'], headline=props.get('headline') or '',
            severity=props.get('severity') or 'Unknown',
            ends=datetime.fromisoformat(props['ends']) if props.get('ends') else None,
        ))
    order = {'Extreme': 0, 'Severe': 1, 'Moderate': 2, 'Minor': 3, 'Unknown': 4}
    return sorted(alerts, key=lambda alert: order.get(alert.severity, 4))
