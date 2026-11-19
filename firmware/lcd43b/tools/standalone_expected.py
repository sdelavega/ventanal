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
import httpx  # noqa: E402
from ventanal.art.sources.aic import AICSource  # noqa: E402
from ventanal.art.sources.cleveland import ClevelandSource  # noqa: E402
from ventanal.art.sources.met import MetSource  # noqa: E402


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


def art_client():
    """Serve the museum fixtures by URL. met-search-winter.json is assembled
    from two real Met searches ("winter": four objects the Met filter drops;
    "winter landscape": print 54428, kept) so both outcomes are covered."""
    def handler(request):
        url = str(request.url)
        if 'api.artic.edu' in url:
            name = 'aic-search-winter.json'
        elif 'clevelandart.org' in url:
            name = 'cleveland-search-winter.json'
        elif '/v1.1/search' in url:
            name = 'met-search-winter.json'
        else:
            name = f'met-object-{url.rsplit("/", 1)[1]}.json'
        return httpx.Response(200, content=(FIXTURES / name).read_bytes())
    return httpx.Client(transport=httpx.MockTransport(handler))


def art_expected():
    client = art_client()
    found = []
    for source, limit in ((AICSource(), 12), (ClevelandSource(), 12), (MetSource(), 5)):
        found += source.search_term('winter', limit, client)
    return {'term': 'winter', 'candidates': [
        {'source': c.source, 'object_id': c.object_id, 'title': c.title, 'artist': c.artist, 'date': c.date,
         'museum': c.museum, 'image_url': c.image_url, 'width': c.width, 'height': c.height} for c in found]}


SOLAR_FILES = {
    'daily-solar-indices.txt': 'swpc-daily.txt', 'noaa-planetary-k-index.json': 'swpc-kp.json',
    'xrays-6-hour.json': 'swpc-xray.json', '3-day-forecast.txt': 'swpc-forecast.txt',
    'alerts.json': 'swpc-alerts.json', 'noaa-scales.json': 'swpc-scales.json',
    'latest_1024_0171.jpg': 'sdo-171.jpg',
}


def solar_now():
    """The newest X-ray reading's time: the moment the captured data describes."""
    from datetime import timezone
    rows = json.loads((FIXTURES / 'swpc-xray.json').read_text())
    stamp = max(r['time_tag'] for r in rows)
    return datetime.fromisoformat(stamp.replace('Z', '+00:00')).astimezone(timezone.utc)


def solar_data(now=None):
    import tempfile
    from ventanal.solar.data import fetch_solar

    def handler(request):
        name = SOLAR_FILES[str(request.url).rsplit('/', 1)[1]]
        return httpx.Response(200, content=(FIXTURES / name).read_bytes())
    with tempfile.TemporaryDirectory() as cache:
        return fetch_solar(httpx.Client(transport=httpx.MockTransport(handler)), Path(cache), now or solar_now())


def solar_expected():
    from ventanal.solar.conditions import conditions, grid_square
    from ventanal.solar.data import forecast_days
    from ventanal.solar.render import alert_summary, forecast_label, integer_k, kp_notation
    now = solar_now()
    s = solar_data(now)
    est = conditions(s.sfi, s.k, s.a, s.xray)
    label, message = alert_summary(s.alerts)
    return {
        'now_epoch': int(now.timestamp()), 'sfi': s.sfi, 'ssn': s.ssn, 'xray': s.xray, 'k': s.k, 'a': s.a,
        'integer_k': integer_k(s.k), 'kp': [[t, k] for t, k in s.kp], 'alerts': s.alerts,
        'scales_now': {key: (s.scales.get('0', {}).get(key, {}) or {}).get('Scale') for key in ('R', 'S', 'G')},
        'forecast': [{'date': d, 'kp_max': k, 'kp_notation': kp_notation(k), 'label': forecast_label(k, g),
                      'r': r, 's': p, 'g': g} for d, k, r, p, g in forecast_days(s)],
        'conditions': {'bands': [list(b) for b in est.bands], 'muf': est.muf, 'noise': est.noise, 'field': est.field},
        'alert_label': label, 'alert_message': message, 'grid_90210': grid_square(34.0901, -118.4065),
    }


def alerts_now():
    """An hour after a real issue time, choosing the moment with the most
    active products, so active_alerts() runs on real messages."""
    from datetime import timedelta, timezone
    from ventanal.solar.data import active_alerts
    rows = json.loads((FIXTURES / 'swpc-alerts.json').read_text())
    best, best_count = None, -1
    for row in rows:
        at = datetime.fromisoformat(row['issue_datetime']).replace(tzinfo=timezone.utc) + timedelta(hours=1)
        count = len(active_alerts(rows, at))
        if count > best_count or (count == best_count and best is not None and at > best):
            best, best_count = at, count
    return best


SUMMARY_CASES = [
    ['WATCH: Geomagnetic Storm Category G1 Predicted · Nov 20 G1, Nov 21 G2'],
    ['WARNING: Geomagnetic K-index of 5 expected · UNTIL Nov 20 0600 UTC'],
    ['ALERT: X-Ray Flux exceeded M5 · Nov 19 1015 UTC', 'ALERT: Electron 2MeV Integral Flux exceeded 1000pfu · Nov 19 1250 UTC'],
    ['ALERT: Geomagnetic K-index of 6 · Nov 19 0300 UTC',
     'WARNING: Geomagnetic K-index of 4 expected · UNTIL Nov 19 2359 UTC',
     'WATCH: Geomagnetic Storm Category G3 Predicted · Nov 20 G3, Nov 21 G2, Nov 22 G1'],
    ['ALERT: Proton Event 10MeV Integral Flux exceeded 10pfu · Nov 19 0720 UTC',
     'ALERT: Type II Radio Emission · Nov 19 0655 UTC'],
]


def solar_alerts_expected():
    from ventanal.solar.data import active_alerts
    from ventanal.solar.render import alert_fit, alert_summary
    now = alerts_now()
    rows = json.loads((FIXTURES / 'swpc-alerts.json').read_text())
    active = active_alerts(rows, now)
    cases = []
    for alerts in [active] + SUMMARY_CASES:
        label, message = alert_summary(alerts)
        fitted, size = alert_fit(message, 550) if label else (message, 14)
        cases.append({'alerts': alerts, 'label': label, 'message': message, 'fitted': fitted})
    return {'now_epoch': int(now.timestamp()), 'active': active, 'cases': cases}


OUTPUTS = {
    'expected-weather-imperial.json': lambda: weather_expected('open-meteo-90210-imperial.json', 'imperial'),
    'expected-weather-metric.json': lambda: weather_expected('open-meteo-90210-metric.json', 'metric'),
    'expected-weather-early.json': lambda: weather_expected('open-meteo-90210-imperial.json', 'imperial', hour=3),
    'expected-alerts-90210.json': lambda: alerts_expected('nws-90210.json'),
    'expected-alerts-ak.json': lambda: alerts_expected('nws-alerts-ak.json'),
    'expected-art-winter.json': art_expected,
    'expected-solar.json': solar_expected,
    'expected-solar-alerts.json': solar_alerts_expected,
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
