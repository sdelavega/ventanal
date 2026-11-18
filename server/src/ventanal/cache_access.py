# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Stephen de la Vega
"""Check cache ownership before entering a command-line writer."""

import os
import stat


def cache_owner_error(directory, command):
    invocation = f'run as: sudo -u ventanal {command}'
    try:
        metadata = directory.stat()
    except FileNotFoundError:
        return f'{directory}: cache directory missing; run server/deploy/install.sh'
    except OSError as exc:
        return f'Unable to access cache directory {directory} ({exc.strerror}); {invocation}'
    if not stat.S_ISDIR(metadata.st_mode):
        return f'{directory} is not a cache directory; check server.cache_dir'
    if metadata.st_uid != os.geteuid():
        return (f'The effective user must own cache directory {directory}; {invocation}. '
                'If the directory is owned by the wrong account, run server/deploy/install.sh.')
    return None
