# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Stephen de la Vega
"""Summarize tagged panel serial evidence; a >5s notification alone is not a stall."""
import argparse
import json
from pathlib import Path
import re
import statistics

STAGES = ('pon', 'master-dtm', 'slave-dtm', 'drf', 'pof')

def parse(lines):
    records = []
    current = None
    for line in lines:
        match = re.match(r'stalltest i=(\d+) screen=(\w+) after=(\w+)', line)
        if match:
            current = dict(i=int(match[1]), screen=match[2], after=match[3],
                           waits={}, entry={}, levels={}, stage=None, outcome='incomplete')
            records.append(current)
        if current is None:
            continue
        match = re.match(r'panel entry busy=(\d+) boot_ms=(\d+) battery=([\d.]+)', line)
        if match:
            current['entry'] = dict(busy=int(match[1]), boot_ms=int(match[2]), battery=float(match[3]))
        match = re.match(r'panel (power-high|reset|screen-init) busy=(\d+)', line)
        if match:
            current['levels'][match[1]] = int(match[2])
        match = re.match(r'busy-enter stage=([\w-]+) level=(\d+)', line)
        if match:
            current['stage'] = match[1]
            current['levels']['wait-' + match[1]] = int(match[2])
        match = re.match(r'busy-done stage=([\w-]+) ms=(\d+)', line)
        if match:
            current['waits'][match[1]] = int(match[2])
        if 'task_wdt: Task watchdog got triggered' in line and current['outcome'] == 'incomplete':
            current['outcome'] = 'watchdog'
        if re.match(r'stalltest complete i=\d+', line):
            current['outcome'] = 'complete'
    return records

def markdown(records):
    text = ['| i | screen | after | pon ms | master-dtm ms | slave-dtm ms | drf ms | pof ms | outcome |',
            '| --- | --- | --- | --- | --- | --- | --- | --- | --- |']
    for r in records:
        values = [r['i'], r['screen'], r['after']] + [r['waits'].get(s, '—') for s in STAGES]
        values += [r['outcome'] + (f" at {r['stage']}" if r['outcome'] != 'complete' else '')]
        text.append('| ' + ' | '.join(map(str, values)) + ' |')
    text += ['', '| Stage | n completed waits | min ms | median ms | max ms |', '| --- | --- | --- | --- | --- |']
    for stage in STAGES:
        values = [r['waits'][stage] for r in records if r['outcome'] == 'complete' and stage in r['waits']]
        if values:
            text.append(f'| {stage} | {len(values)} | {min(values)} | {statistics.median(values):g} | {max(values)} |')
    return '\n'.join(text) + '\n'

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('log', type=Path)
    parser.add_argument('--json', action='store_true')
    args = parser.parse_args()
    records = parse(args.log.read_text().splitlines())
    print(json.dumps(records, indent=2) if args.json else markdown(records), end='\n')
