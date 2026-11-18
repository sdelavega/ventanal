# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Stephen de la Vega

from dataclasses import FrozenInstanceError
from datetime import date, time
from pathlib import Path

import pytest

from ventanal.config import ConfigError, load_config


EXAMPLE = Path(__file__).parents[1] / "ventanal.example.toml"


def changed_config(tmp_path, old, new):
    path = tmp_path / "config.toml"
    text = EXAMPLE.read_text()
    assert old in text
    path.write_text(text.replace(old, new))
    return path


def test_example_loads():
    from ventanal.art.pool import season_for

    cfg = load_config(EXAMPLE)
    assert cfg.art_slots[0] == time(2, 0)
    assert len(cfg.art_slots) == 6
    assert cfg.panel_rgb[2] == (0xee, 0xd8, 0x45)
    assert cfg.listen_port == 8765
    assert cfg.seasons[0].start == (11, 1)
    assert [s.name for s in cfg.seasons] == [
        "holiday-winter", "winter", "spring", "summer", "autumn"
    ]
    assert cfg.output_dir == Path("/var/lib/ventanal")
    assert cfg.cache_dir == Path("/var/cache/ventanal")
    assert season_for(date(2028, 2, 29), cfg.seasons).name == "winter"
    with pytest.raises(FrozenInstanceError):
        cfg.zip = "12345"


def test_missing_location_rejected(tmp_path):
    path = changed_config(tmp_path, 'zip = "00000"', '# no ZIP')
    with pytest.raises(ConfigError, match=r"location\."):
        load_config(path)


def test_bad_palette_rejected(tmp_path):
    path = changed_config(tmp_path, 'yellow = "#eed845"', 'yellow = "nope"')
    with pytest.raises(ConfigError, match=r"panel\.yellow"):
        load_config(path)


def test_zip_plus4_kept(tmp_path):
    path = changed_config(tmp_path, 'zip = "00000"', 'zip = "32084-1234"')
    assert load_config(path).zip == "32084-1234"


def test_coordinates_and_wrapping_season(tmp_path):
    path = changed_config(
        tmp_path, 'zip = "00000"',
        'latitude = 29.9144193\nlongitude = -81.3628995\nplace_name = "St. Augustine"',
    )
    path.write_text(path.read_text().replace('end = "12-31"', 'end = "02-28"'))
    cfg = load_config(path)
    assert (cfg.zip, cfg.latitude, cfg.longitude, cfg.place_name) == (None, 29.9144193, -81.3628995, "St. Augustine")
    assert cfg.seasons[0].end == (2, 28)


@pytest.mark.parametrize(('old', 'new', 'gap'), [
    ('end = "02-29"', 'end = "02-28"', '02-29'),
    ('start = "03-01"', 'start = "03-02"', '03-01'),
])
def test_uncovered_season_dates_rejected(tmp_path, old, new, gap):
    with pytest.raises(ConfigError, match=f'art.seasons: no season covers {gap}'):
        load_config(changed_config(tmp_path, old, new))


@pytest.mark.parametrize(("old", "new", "key"), [
    ('zip = "00000"', 'zip = 32084', "location.zip"),
    ('zip = "00000"', 'latitude = 91\nlongitude = 0', "location.latitude"),
    ('zip = "00000"', 'latitude = 35', "location.longitude"),
    ('units = "imperial"', 'units = "kelvin"', "display.units"),
    ('clock = "24h"', 'clock = "13h"', "display.clock"),
    ('weather_interval_minutes = 30', 'weather_interval_minutes = 0', "schedule.weather_interval_minutes"),
    ('"02:00"', '"24:00"', "schedule.art_slots"),
    ('no_repeat_days = 60', 'no_repeat_days = true', "art.no_repeat_days"),
    ('start = "11-01"', 'start = "02-30"', "art.seasons"),
    ('radar = "iem"', 'radar = "other"', "weather.radar"),
    ('listen = "0.0.0.0:8765"', 'listen = "localhost:99999"', "server.listen"),
])
def test_invalid_values_name_the_key(tmp_path, old, new, key):
    with pytest.raises(ConfigError) as error:
        load_config(changed_config(tmp_path, old, new))
    assert str(error.value).startswith(key)


def test_malformed_toml(tmp_path):
    path = tmp_path / "broken.toml"
    path.write_text("[location")
    with pytest.raises(ConfigError):
        load_config(path)

@pytest.mark.parametrize('label', ['off', 'en', 'es'])
def test_here_label(tmp_path, label):
    path = changed_config(tmp_path, 'here_label = "off"', f'here_label = "{label}"')
    assert load_config(path).here_label == label


def test_here_label_default(tmp_path):
    path = changed_config(tmp_path, 'here_label = "off"', '')
    assert load_config(path).here_label == 'off'


@pytest.mark.parametrize('value', ['"fr"', 'true', '1'])
def test_here_label_invalid(tmp_path, value):
    path = changed_config(tmp_path, 'here_label = "off"', f'here_label = {value}')
    with pytest.raises(ConfigError, match='weather.here_label'):
        load_config(path)
