#!/usr/bin/env python3
"""Monitor the device while an operator performs Wi-Fi/map/camera fault injection."""

import argparse
from pathlib import Path

from hil_common import capture, current_git_revision, validate, validate_event_counts


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", required=True)
    parser.add_argument("--duration", type=int, default=3600)
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--output", type=Path, default=Path("hil-network.log"))
    parser.add_argument("--wifi-recoveries", type=int, default=20)
    parser.add_argument("--map-cycles", type=int, default=100)
    parser.add_argument("--camera-cycles", type=int, default=100)
    parser.add_argument("--revision", default="")
    args = parser.parse_args()
    print("Exercise AP loss/recovery, map refresh and camera open/close during this window.")
    text = capture(args.port, args.baud, args.duration, args.output)
    validate(text, args.duration,
             expected_revision=args.revision or current_git_revision(),
             require_fresh_boot=True, expect_music_stress=False)
    validate_event_counts(text, {
        "wifi_loss": args.wifi_recoveries,
        "wifi_recovery": args.wifi_recoveries,
        "map_open": args.map_cycles,
        "map_close": args.map_cycles,
        "map_request": args.map_cycles,
        "map_publish": args.map_cycles,
        "camera_open": args.camera_cycles,
        "camera_close": args.camera_cycles,
        "camera_frame": args.camera_cycles,
    })
    print(f"Network HIL serial and health gates passed; log: {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
