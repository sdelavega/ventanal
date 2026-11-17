# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Stephen de la Vega
import importlib.util
from pathlib import Path

spec = importlib.util.spec_from_file_location('stall_log', Path(__file__).parents[1] / 'tools/diag/analyze.py')
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)

def test_threshold_is_not_stall_and_reset_closes_attempt():
    records = module.parse('''stalltest i=1 screen=art after=pause
panel entry busy=0 boot_ms=123 battery=3.980
panel power-high busy=0
panel reset busy=1
panel screen-init busy=1
busy-enter stage=drf level=0
busy-stuck stage=drf
busy-done stage=drf ms=20000
stalltest complete i=1
stalltest i=2 screen=weather after=pause
panel entry busy=0 boot_ms=30000 battery=3.981
busy-enter stage=pon level=0
busy-stuck stage=pon
E (60000) task_wdt: Task watchdog got triggered.
reset=watchdog count=1
stalltest i=3 screen=art after=pause
'''.splitlines())
    assert records[0]['outcome'] == 'complete'
    assert records[0]['waits'] == {'drf': 20000}
    assert records[1]['outcome'] == 'watchdog'
    assert records[1]['stage'] == 'pon'
    assert records[1]['entry']['boot_ms'] == 30000
    assert records[2]['outcome'] == 'incomplete'
