#!/usr/bin/env python3
"""Monitor the device while an operator performs Wi-Fi/map/camera fault injection."""

import argparse
from pathlib import Path

from hil_common import capture, validate


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", required=True)
    parser.add_argument("--duration", type=int, default=3600)
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--output", type=Path, default=Path("hil-network.log"))
    args = parser.parse_args()
    print("Exercise AP loss/recovery, map refresh and camera open/close during this window.")
    text = capture(args.port, args.baud, args.duration, args.output)
    validate(text, args.duration)
    print(f"Network HIL serial and health gates passed; log: {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
