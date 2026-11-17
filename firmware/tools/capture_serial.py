# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Stephen de la Vega
"""Capture a bounded 115200-baud bring-up session (opening may reset board)."""
import argparse
import time

import serial

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--port", default="/dev/cu.wchusbserial10")
parser.add_argument("--seconds", type=float, default=100)
parser.add_argument("--reset", action="store_true", help="Reset via CH340 RTS after opening")
parser.add_argument("--until", help="Stop after a line contains this text")
args = parser.parse_args()
connection = serial.Serial(None, 115200, timeout=0.25)
connection.dtr = False
connection.rts = False
connection.port = args.port
with connection as port:
    if args.reset:
        port.dtr = False
        port.rts = True
        time.sleep(0.1)
        port.rts = False
    deadline = time.monotonic() + args.seconds
    while time.monotonic() < deadline:
        line = port.readline()
        if line:
            text = line.decode("utf-8", errors="replace").rstrip()
            print(text, flush=True)
            if args.until and args.until in text:
                break
