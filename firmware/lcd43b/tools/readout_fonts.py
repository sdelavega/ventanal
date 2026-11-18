# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Stephen de la Vega
"""Compatibility entry point for the LCD's 4-bit coverage font generator."""
from pathlib import Path
import runpy
runpy.run_path(str(Path(__file__).resolve().parents[3] / 'tools/aa_fonts.py'),
               run_name='__main__')
