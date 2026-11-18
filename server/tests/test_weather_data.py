# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Stephen de la Vega

from copy import deepcopy
from dataclasses import replace
from datetime import datetime, timedelta, timezone
import json
from pathlib import Path
from zoneinfo import ZoneInfo

import httpx
import pytest

from ventanal.config import load_config
from ventanal.weather.data import (
    WMO_LONG, WMO_SHORT, compass8, fetch_alerts, fetch_weather, geocode,
)

FIXTURES = Path(__file__).parent / 'fixtures'
USER_AGENT = 'Ventanal/1.0.0'


def fixture(name):
    return json.loads((FIXTURES / name).read_text())


def client_for(data, requests, status=200):
    def respond(request):
        requests.append(request)
        return httpx.Response(status, json=data)
    return httpx.Client(transport=httpx.MockTransport(respond))


def config(**kwargs):
    return replace(load_config(Path(__file__).parents[1] / 'ventanal.example.toml'), **kwargs)


def test_geocode_uses_5_digits(tmp_path):
    requests = []
    with client_for(fixture('zippopotam_80302.json'), requests) as client:
        result = geocode(config(zip='80302-1234'), client, tmp_path)
        assert requests[0].url.path.endswith('/us/80302')
        assert requests[0].headers['User-Agent'] == USER_AGENT
        assert result == (40.01, -105.27, 'BOULDER · CO')
        assert (tmp_path / 'geocode.json').is_file()
        assert geocode(config(zip='80302'), client, tmp_path) == result
        assert geocode(config(zip='80302', place_name='My place'), client, tmp_path)[2] == 'My place'
        assert len(requests) == 1
        geocode(config(zip='10001'), client, tmp_path)
        assert len(requests) == 2


def test_geocode_coordinates_and_override(tmp_path):
    requests = []
    with client_for({}, requests) as client:
        assert geocode(config(latitude=0, longitude=0, place_name='Equator'), client, tmp_path) == (0, 0, 'Equator')
        assert geocode(config(latitude=0, longitude=0, place_name=None), client, tmp_path)[:2] == (0, 0)
    assert requests == []


def test_geocode_corrupt_cache_recovers(tmp_path):
    (tmp_path / 'geocode.json').write_text('{broken')
    with client_for(fixture('zippopotam_80302.json'), []) as client:
        assert geocode(config(zip='80302'), client, tmp_path)[2] == 'BOULDER · CO'


@pytest.mark.parametrize('units', ['imperial', 'metric'])
def test_weather_parse(units):
    data = fixture('openmeteo.json')
    now = datetime.fromisoformat(data['current']['time']).replace(tzinfo=ZoneInfo(data['timezone']))
    requests = []
    with client_for(data, requests) as client:
        weather = fetch_weather(40.7484, -73.9967, units, client, now=now)
    assert len(weather.days) == 7
    assert type(weather.days[0].hi) is int
    assert type(weather.days[0].lo) is int
    assert weather.current.wind_dir in {'N', 'NE', 'E', 'SE', 'S', 'SW', 'W', 'NW'}
    assert weather.current.temp == data['current']['temperature_2m']
    assert weather.current.humidity == data['current']['relative_humidity_2m']
    assert weather.sunrise.tzinfo == ZoneInfo(data['timezone'])
    assert weather.sunset.utcoffset() is not None
    assert weather.fetched == now
    params = requests[0].url.params
    assert requests[0].headers['User-Agent'] == USER_AGENT
    assert str(requests[0].url).startswith('https://api.open-meteo.com/v1/forecast?')
    assert params['current'] == 'temperature_2m,relative_humidity_2m,weather_code,wind_speed_10m,wind_direction_10m'
    assert params['hourly'] == 'precipitation_probability'
    assert params['daily'] == 'weather_code,temperature_2m_max,temperature_2m_min,precipitation_probability_max,sunrise,sunset'
    assert params['timezone'] == 'auto'
    assert params['forecast_days'] == '7'
    if units == 'imperial':
        assert params['temperature_unit'] == 'fahrenheit'
        assert params['wind_speed_unit'] == 'mph'
    else:
        assert 'temperature_unit' not in params
        assert 'wind_speed_unit' not in params


@pytest.mark.parametrize('probabilities, expected', [
    ([100, 100, 39, 40, 100], 3),
    ([100, 100, 39, 0, 100], None),
    ([100, 100, None, 40, 100], 3),
])
def test_rain_onset_only_future_today(probabilities, expected):
    data = deepcopy(fixture('openmeteo.json'))
    zone = ZoneInfo(data['timezone'])
    day = data['daily']['time'][0]
    times = [f'{day}T09:00', f'{day}T10:00', f'{day}T11:00', f'{day}T12:00',
             (datetime.fromisoformat(day) + timedelta(days=1)).isoformat(timespec='minutes')]
    data['hourly'] = {'time': times, 'precipitation_probability': probabilities}
    now = datetime.fromisoformat(times[1]).replace(tzinfo=zone)
    with client_for(data, []) as client:
        result = fetch_weather(40.7484, -73.9967, 'metric', client, now=now.astimezone(timezone.utc))
    assert result.rain_onset == (datetime.fromisoformat(times[expected]).replace(tzinfo=zone) if expected else None)


def test_weather_default_now_is_aware():
    before = datetime.now(timezone.utc)
    with client_for(fixture('openmeteo.json'), []) as client:
        weather = fetch_weather(40.7484, -73.9967, 'imperial', client)
    assert before <= weather.fetched <= datetime.now(timezone.utc)


@pytest.mark.parametrize('contact', ['', 'weather@example.com'])
def test_alerts_sorted(contact):
    requests = []
    with client_for(fixture('nws_alerts.json'), requests) as client:
        alerts = fetch_alerts(40.7484, -73.9967, client, contact)
    assert [alert.severity for alert in alerts] == ['Severe', 'Minor']
    assert alerts[0].ends.utcoffset() is not None
    assert alerts[1].ends is None
    assert alerts[0].event == 'Test severe event'
    assert alerts[0].headline == 'Synthetic severe headline'
    assert requests[0].url.params['point'] == '40.7484,-73.9967'
    assert requests[0].headers['User-Agent'] == USER_AGENT + (f' <{contact}>' if contact else '')
    assert requests[0].headers['Accept'] == 'application/geo+json'
    with client_for(fixture('nws_alerts_empty.json'), []) as client:
        assert fetch_alerts(40.7484, -73.9967, client, contact) == []


def test_all_alert_severities():
    data = fixture('nws_alerts.json')
    template = data['features'][0]
    data['features'] = [dict(template, properties=dict(template['properties'], severity=severity))
                        for severity in ['Unknown', 'Minor', 'Moderate', 'Severe', 'Extreme']]
    with client_for(data, []) as client:
        assert [a.severity for a in fetch_alerts(40.7484, -73.9967, client, '')] == ['Extreme', 'Severe', 'Moderate', 'Minor', 'Unknown']


def test_wmo_tables_cover():
    codes = {0,1,2,3,45,48,51,53,55,56,57,61,63,65,66,67,71,73,75,77,80,81,82,85,86,95,96,99}
    for code in codes:
        assert WMO_LONG[code]
        assert 0 < len(WMO_SHORT[code]) <= 8


@pytest.mark.parametrize('degree, expected', [(0,'N'), (22.49,'N'), (22.5,'NE'), (90,'E'), (135,'SE'), (180,'S'), (225,'SW'), (270,'W'), (315,'NW'), (337.5,'N'), (360,'N'), (-45,'NW')])
def test_compass8(degree, expected):
    assert compass8(degree) == expected


@pytest.mark.parametrize('operation', ['geocode', 'weather', 'alerts'])
def test_http_errors_propagate(operation, tmp_path):
    with client_for({}, [], status=503) as client, pytest.raises(httpx.HTTPStatusError):
        if operation == 'geocode':
            geocode(config(zip='80302'), client, tmp_path)
        elif operation == 'weather':
            fetch_weather(40.7484, -73.9967, 'metric', client)
        else:
            fetch_alerts(40.7484, -73.9967, client, '')
