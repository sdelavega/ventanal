# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Stephen de la Vega

"""Ventanal e-paper render service."""

# api.weather.gov answers 403 to any User-Agent containing the repository
# path "sdelavega/ventanal" (measured 2026-11-17), so link the profile instead.
USER_AGENT = "Ventanal/1.0.0 (+https://github.com/sdelavega)"
