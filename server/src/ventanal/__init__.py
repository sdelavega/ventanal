# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Stephen de la Vega

"""Ventanal e-paper render service."""

# Keep the User-Agent free of links: api.weather.gov refuses agents containing
# "sdelavega/ventanal" and the Art Institute of Chicago image server refuses
# any agent mentioning github.com (both measured 2026-11-17).
USER_AGENT = "Ventanal/1.0.0"
