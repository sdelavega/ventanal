# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Stephen de la Vega
"""Expected standalone weather values, computed by the server's own parsers.

The board's Open-Meteo and NWS parsers (firmware/lcd43b/local/) must match
the server field for field. This runs ventanal.weather.data.fetch_weather and
fetch_alerts over the captured public fixtures (sample ZIP 90210, a few
Alaska alerts) and writes the results next to them.

Run with server/.venv/bin/python firmware/lcd43b/tools/standalone_expected.py [--check].
"""
import argparse
import json
import sys
from datetime import datetime
from pathlib import Path
from zoneinfo import ZoneInfo

ROOT = Path(__file__).resolve().parents[3]
FIXTURES = ROOT / 'firmware/test/fixtures/standalone'
sys.path.insert(0, str(ROOT / 'server/src'))
from ventanal.weather.data import fetch_alerts, fetch_weather  # noqa: E402


class FixtureClient:
    def __init__(self, path):
        self.data = json.loads(Path(path).read_text())

    def get(self, *args, **kwargs):
        client = self

        class Response:
            def raise_for_status(self):
                pass

            def json(self):
                return client.data
        return Response()


def fixture_now(path):
    """The fixture's own current time, in its zone: what the board's clock reads."""
    data = json.loads(Path(path).read_text())
    return datetime.fromisoformat(data['current']['time']).replace(tzinfo=ZoneInfo(data['timezone']))


def weather_expected(name, units, hour=None):
    path = FIXTURES / name
    now = fixture_now(path)
    if hour is not None:  # an earlier clock on the same day, so today's rain is still ahead
        now = now.replace(hour=hour, minute=0)
    w = fetch_weather(34.0901, -118.4065, units, FixtureClient(path), now=now)
    return {
        'fixture': name, 'units': units, 'now_epoch': int(now.timestamp()),
        'current': {'temp': w.current.temp, 'humidity': w.current.humidity,
                    'wind_speed': w.current.wind_speed, 'wind_dir': w.current.wind_dir,
                    'code': w.current.code},
        'days': [{'date': d.date.isoformat(), 'weekday': d.date.strftime('%a').upper(), 'hi': d.hi,
                  'lo': d.lo, 'code': d.code, 'precip': d.precip_prob} for d in w.days],
        'sunrise': w.sunrise.strftime('%H:%M'), 'sunset': w.sunset.strftime('%H:%M'),
        'rain_onset': w.rain_onset.strftime('%Y-%m-%dT%H:%M') if w.rain_onset else None,
        'hours': [{'time': h.time.strftime('%Y-%m-%dT%H:%M'), 'temp': h.temp, 'precip': h.precip_prob}
                  for h in w.hours],
    }


def alerts_expected(name):
    alerts = fetch_alerts(34.0901, -118.4065, FixtureClient(FIXTURES / name), '')
    return {'fixture': name, 'alerts': [
        {'event': a.event, 'headline': a.headline, 'severity': a.severity,
         'ends_epoch': int(a.ends.timestamp()) if a.ends else None} for a in alerts]}


OUTPUTS = {
    'expected-weather-imperial.json': lambda: weather_expected('open-meteo-90210-imperial.json', 'imperial'),
    'expected-weather-metric.json': lambda: weather_expected('open-meteo-90210-metric.json', 'metric'),
    'expected-weather-early.json': lambda: weather_expected('open-meteo-90210-imperial.json', 'imperial', hour=3),
    'expected-alerts-90210.json': lambda: alerts_expected('nws-90210.json'),
    'expected-alerts-ak.json': lambda: alerts_expected('nws-alerts-ak.json'),
}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--check', action='store_true', help='fail if an expected file is stale')
    args = parser.parse_args()
    for name, build in OUTPUTS.items():
        text = json.dumps(build(), indent=1, ensure_ascii=False) + '\n'
        path = FIXTURES / name
        if args.check:
            if not path.is_file() or path.read_text() != text:
                sys.exit(f'Stale generated file: {path.relative_to(ROOT)}')
        else:
            path.write_text(text)


if __name__ == '__main__':
    main()
