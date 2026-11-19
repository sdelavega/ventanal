# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Stephen de la Vega
"""Server renders of the standalone screens, from the captured fixtures.

These are the reference ("golden") images the board's renderer must match;
they are rendered fresh from the server's code, never stored.

Run with server/.venv/bin/python firmware/lcd43b/tools/standalone_render.py OUT_DIR
"""
import sys
from pathlib import Path
from types import SimpleNamespace

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / 'server/src'))
sys.path.insert(0, str(Path(__file__).parent))
from standalone_expected import FIXTURES, FixtureClient, fixture_now  # noqa: E402
from ventanal.devices.lcd_weather import render_lcd_weather  # noqa: E402
from ventanal.weather.data import fetch_alerts, fetch_weather  # noqa: E402

LAT, LON, PLACE = 34.0901, -118.4065, 'Beverly Hills · CA'

# name -> (forecast fixture, units, clock_24h, alerts fixture, hour override)
WEATHER_CASES = {
    'weather-imperial-12h': ('open-meteo-90210-imperial.json', 'imperial', False, 'nws-90210.json', None),
    'weather-metric-24h': ('open-meteo-90210-metric.json', 'metric', True, 'nws-alerts-ak.json', None),
    'weather-early-quiet': ('open-meteo-90210-imperial.json', 'imperial', False, None, 3),
}


def weather_case(name):
    forecast, units, clock_24h, alerts_name, hour = WEATHER_CASES[name]
    path = FIXTURES / forecast
    now = fixture_now(path)
    if hour is not None:
        now = now.replace(hour=hour, minute=0)
    w = fetch_weather(LAT, LON, units, FixtureClient(path), now=now)
    alerts = fetch_alerts(LAT, LON, FixtureClient(FIXTURES / alerts_name), '') if alerts_name else []
    cfg = SimpleNamespace(units=units, clock_24h=clock_24h, place_name=PLACE, latitude=LAT, longitude=LON)
    return render_lcd_weather(w, alerts, None, cfg, now, place=PLACE, coords=(LAT, LON), standalone=True)


def main():
    out = Path(sys.argv[1])
    out.mkdir(parents=True, exist_ok=True)
    for name in WEATHER_CASES:
        weather_case(name).save(out / f'{name}.png')


if __name__ == '__main__':
    main()
