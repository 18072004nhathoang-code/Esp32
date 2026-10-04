#!/usr/bin/env python3
"""Reset an ESP32 serial port and capture bounded boot diagnostics."""

import argparse
from contextlib import ExitStack
import sys
import time

import serial


def main() -> int:
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", required=True)
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--seconds", type=float, default=20.0)
    parser.add_argument("--no-reset", action="store_true",
                        help="Observe an ongoing hardware test without toggling reset")
    parser.add_argument("--output", help="Save exact serial bytes for backtrace decoding")
    args = parser.parse_args()

    # Observation must set lines before opening. For a deliberate reset keep
    # pyserial's initial asserted lines, then use the board-tested transition.
    # Some native USB bridges do not reset from an initial deasserted DTR.
    connection = serial.Serial(port=None, baudrate=args.baud, timeout=0.1)
    if args.no_reset:
        connection.dtr = False
        connection.rts = False
    connection.port = args.port
    with ExitStack() as stack:
        connection.open()
        stack.enter_context(connection)
        capture = stack.enter_context(open(args.output, "wb")) if args.output else None
        if not args.no_reset:
            connection.dtr = False
            connection.rts = True
            time.sleep(0.1)
        connection.rts = False
        deadline = time.monotonic() + args.seconds
        while time.monotonic() < deadline:
            chunk = connection.read(4096)
            if chunk:
                if capture:
                    capture.write(chunk)
                    capture.flush()
                sys.stdout.write(chunk.decode("utf-8", errors="replace"))
                sys.stdout.flush()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
