#!/usr/bin/env python3
"""Reset an ESP32 serial port and capture bounded boot diagnostics."""

import argparse
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
    args = parser.parse_args()

    with serial.Serial(args.port, args.baud, timeout=0.1) as connection:
        connection.dtr = False
        connection.rts = True
        time.sleep(0.1)
        connection.rts = False
        deadline = time.monotonic() + args.seconds
        while time.monotonic() < deadline:
            chunk = connection.read(4096)
            if chunk:
                sys.stdout.write(chunk.decode("utf-8", errors="replace"))
                sys.stdout.flush()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
