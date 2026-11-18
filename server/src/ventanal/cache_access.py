# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Stephen de la Vega
"""Check cache ownership before entering a command-line writer."""

import os


def cache_owner_error(directory, command):
    invocation = f'run as: sudo -u ventanal {command}'
    try:
        directory.mkdir(parents=True, exist_ok=True)
        owner = directory.stat().st_uid
    except OSError as exc:
        return f'Unable to access cache directory {directory} ({exc.strerror}); {invocation}'
    if owner != os.geteuid():
        return f'The effective user must own cache directory {directory}; {invocation}'
    return None
