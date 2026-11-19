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
from standalone_expected import FIXTURES, SUMMARY_CASES, FixtureClient, alerts_now, fixture_now, solar_data, solar_now  # noqa: E402
from ventanal.devices.lcd43b import render_art  # noqa: E402
from ventanal.devices.lcd_weather import render_lcd_weather  # noqa: E402
from ventanal.devices.lcd_solar import render_lcd_solar  # noqa: E402
from PIL import Image  # noqa: E402
import json  # noqa: E402
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


# name -> image fixture art-<source>-<object id>.jpg, at the size the board
# asks for; the credit comes from that candidate in expected-art-winter.json.
ART_CASES = {
    'art-cover-aic': ('art-aic-10932.jpg', None),         # 3:2, within 15 % of 4:3: cover crop
    'art-wide-cleveland': ('art-cleveland-153737.jpg', None),  # very wide: backdrop + contained
    'art-wide-met': ('art-met-54428.jpg', None),          # very wide, small image
}


def art_credit(name):
    """The credit the board builds from the same candidate (expected-art-winter.json)."""
    image, _ = ART_CASES[name]
    source, object_id = image[4:-4].split('-')
    expected = json.loads((FIXTURES / 'expected-art-winter.json').read_text())
    c = next(c for c in expected['candidates'] if c['source'] == source and c['object_id'] == object_id)
    return {key: c[key] for key in ('title', 'artist', 'date', 'museum')}


def art_source(name):
    with Image.open(FIXTURES / ART_CASES[name][0]) as im:
        return im.convert('RGB')


def art_case(name):
    return render_art(art_source(name), art_credit(name))


def write_art_inputs(out):
    """The decoded pixels (PPM) and credit (JSON) the board's host render uses."""
    for name in ART_CASES:
        art_source(name).save(out / f'{name}-input.ppm')
        (out / f'{name}-credit.json').write_text(json.dumps(art_credit(name)))


# name -> alerts on the screen: None keeps the capture's own (none active).
# The call sign is a placeholder; the board shows the one set up on it.
SOLAR_CALL = 'N0CALL'
SOLAR_CASES = {
    'solar-quiet': None,
    'solar-alerts': 'active',     # five live products at alerts_now()
    'solar-long-alert': 3,        # SUMMARY_CASES[3]: elided to fit
}


def solar_alerts(name):
    choice = SOLAR_CASES[name]
    if choice is None:
        return None
    if choice == 'active':
        from ventanal.solar.data import active_alerts
        return active_alerts(json.loads((FIXTURES / 'swpc-alerts.json').read_text()), alerts_now())
    return SUMMARY_CASES[choice]


def solar_case(name):
    from dataclasses import replace
    now = solar_now()
    data = solar_data(now)
    alerts = solar_alerts(name)
    if alerts is not None:
        data = replace(data, alerts=alerts)
    cfg = SimpleNamespace(ham_call_sign=SOLAR_CALL, latitude=LAT, longitude=LON)
    return render_lcd_solar(data, cfg, now)


def write_solar_inputs(out):
    """The SDO image at the board's decode scale (TJpgDec at 1/4), and each
    case's alerts (JSON list, or null for the capture's own)."""
    with Image.open(FIXTURES / 'sdo-171.jpg') as im:
        im.convert('RGB').reduce(4).save(out / 'solar-sun-input.ppm')
    for name in SOLAR_CASES:
        (out / f'{name}-alerts.json').write_text(json.dumps(solar_alerts(name)))


def main():
    out = Path(sys.argv[1])
    out.mkdir(parents=True, exist_ok=True)
    for name in WEATHER_CASES:
        weather_case(name).save(out / f'{name}.png')
    for name in ART_CASES:
        art_case(name).save(out / f'{name}.png')
    for name in SOLAR_CASES:
        solar_case(name).save(out / f'{name}.png')
    if len(sys.argv) > 2 and sys.argv[2] == '--inputs':
        write_art_inputs(out)
        write_solar_inputs(out)


if __name__ == '__main__':
    main()
