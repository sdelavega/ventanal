#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Score a timestamped serial capture from a Ventanal board."""

import argparse
import json
import re
from pathlib import Path


TIMESTAMP = re.compile(r"^([01]\d|2[0-3]):([0-5]\d):([0-5]\d)\s+")
BOOT = re.compile(r"\bboot\s+fw=\S+")
REFRESH = re.compile(r"^refresh fetch=\d+ ok=\d+ offline=\d+ failed=\d+(?:\s|$)")
STANDALONE = re.compile(r"^standalone frame slot=(weather|solar)\s+id=\S+")
ART = re.compile(r"^cache queued slot=(?:art|spare0|spare1)(?:\s|$)")
FRAME = re.compile(r"^cache persist slot=\S+\s+id=\S+.*?\sok=1(?:\s|$)")
RESUME = re.compile(r"^resume target=(\S+)\s+from=(\d+)\s+total=(\d+)(?:\s|$)")
RESUME_DONE = re.compile(r"^resume done target=(\S+)\s+bytes=(\d+)(?:\s|$)")
FRAME_RESULT = re.compile(r"^frame slot=(\S+)\s+outcome=(\S+).*?\sgot=(\d+)\s+have=(\d+)\s+total=(\d+)(?:\s|$)")


def score(lines):
    completed = dict.fromkeys(("weather", "solar", "art", "frames"), 0)
    result = {
        "passes": 0,
        "completed": completed,
        "failed_labels": 0,
        "reboots": 0,
        "watchdogs": 0,
        "resumes": 0,
        "resume_bytes_skipped": 0,
        "max_pass_s": 0.0,
    }
    boot_seen = False
    previous_end = None
    last_clock = None
    day_offset = 0
    pending = {}
    for line in lines:
        line = line.strip()
        timestamp = TIMESTAMP.match(line)
        now = None
        if timestamp:
            hours, minutes, seconds = map(int, timestamp.groups())
            clock = hours * 3600 + minutes * 60 + seconds
            if last_clock is not None and clock < last_clock:
                day_offset += 86400
            last_clock = clock
            now = day_offset + clock
            line = line[timestamp.end():]

        if BOOT.search(line):
            if boot_seen:
                result["reboots"] += 1
            boot_seen = True
            previous_end = now
        if line == "label=DOWNLOAD FAILED":
            result["failed_labels"] += 1
        if "task_wdt" in line or "Task watchdog" in line:
            result["watchdogs"] += 1

        if REFRESH.match(line):
            result["passes"] += 1
            if now is not None and previous_end is not None:
                result["max_pass_s"] = max(result["max_pass_s"], float(now - previous_end))
            previous_end = now
        elif match := STANDALONE.match(line):
            completed[match[1]] += 1
        elif ART.match(line):
            completed["art"] += 1
        elif FRAME.match(line):
            completed["frames"] += 1
        elif match := RESUME.match(line):
            pending[match[1]] = (int(match[2]), int(match[3]), False)
        elif match := RESUME_DONE.match(line):
            target, size = match[1], int(match[2])
            if target in pending:
                offset, total, _ = pending[target]
                if size == total:
                    if target.startswith("frame-"):
                        pending[target] = (offset, total, True)
                    else:
                        result["resumes"] += 1
                        result["resume_bytes_skipped"] += offset
                        del pending[target]
        elif match := FRAME_RESULT.match(line):
            target = "frame-" + match[1]
            if target in pending:
                offset, total, done = pending.pop(target)
                if done and match[2] == "complete" and int(match[5]) == total and int(match[3]) == total - offset and int(match[4]) == total:
                    result["resumes"] += 1
                    result["resume_bytes_skipped"] += offset

    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", type=Path)
    args = parser.parse_args()
    with args.log.open(encoding="utf-8", errors="replace") as log:
        result = score(log)
    print(json.dumps(result))
    return int(any(result[key] for key in ("failed_labels", "reboots", "watchdogs")))


if __name__ == "__main__":
    raise SystemExit(main())
