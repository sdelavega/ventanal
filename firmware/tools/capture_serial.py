# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Stephen de la Vega
"""Capture a bounded session without toggling modem lines on native USB CDC."""
import argparse
from contextlib import contextmanager
import os
import time


@contextmanager
def capture_port(name, reset):
    if not reset and os.path.basename(name).startswith(("cu.usbmodem", "tty.usbmodem", "ttyACM")):
        import fcntl
        import select
        import termios

        # A pyserial open drives DTR and RTS through separate ioctls even
        # when both are False. On the ESP32-S3 that can signal a reset.
        fd = os.open(name, os.O_RDONLY | os.O_NOCTTY | os.O_NONBLOCK)
        try:
            fcntl.ioctl(fd, termios.TIOCEXCL)
            def read_chunk(timeout):
                if not select.select([fd], [], [], timeout)[0]:
                    return b""
                try:
                    return os.read(fd, 4096) or None
                except BlockingIOError:
                    return b""

            yield read_chunk
        finally:
            os.close(fd)
    else:
        import serial

        connection = serial.Serial(port=None, baudrate=115200, timeout=0.25,
                                   dsrdtr=False, rtscts=False, exclusive=True)
        connection.dtr = False
        connection.rts = False
        connection.port = name
        with connection as port:
            if reset:
                port.rts = True
                time.sleep(0.1)
                port.rts = False
            def read_chunk(timeout):
                port.timeout = timeout
                return port.read(min(4096, max(1, port.in_waiting)))

            yield read_chunk


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", default="/dev/cu.wchusbserial10")
    parser.add_argument("--seconds", type=float, default=100)
    parser.add_argument("--reset", action="store_true", help="Reset via CH340 RTS after opening")
    parser.add_argument("--until", help="Stop after a line contains this text")
    args = parser.parse_args()
    with capture_port(args.port, args.reset) as read_chunk:
        deadline = time.monotonic() + args.seconds
        leftover = b""
        while time.monotonic() < deadline:
            chunk = read_chunk(min(0.25, max(0, deadline - time.monotonic())))
            if chunk is None:
                break
            if not chunk:
                continue
            *lines, leftover = (leftover + chunk).split(b"\n")
            for line in lines:
                text = line.decode("utf-8", errors="replace").rstrip()
                print(text, flush=True)
                if args.until and args.until in text:
                    return


if __name__ == "__main__":
    main()
