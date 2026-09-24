#!/usr/bin/env python3
"""Monitor the operator-driven eight-hour reference-board acceptance soak."""

import argparse
from pathlib import Path

from hil_common import capture, parse_health, validate


# Release firmware task IDs 0..12. MusicStress (ID 13) belongs only to the
# dedicated stress environment and is intentionally excluded here.
RELEASE_REQUIRED_TASKS_MASK = (1 << 13) - 1


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", required=True)
    parser.add_argument("--duration", type=int, default=28800)
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--output", type=Path, default=Path("hil-acceptance.log"))
    args = parser.parse_args()
    print("Run the full checklist in docs/RELEASE_CHECKLIST.md during this window.")
    print("Required short-lived tasks: save Settings, export one recording to SD, "
          "and complete the speaker self-test.")
    text = capture(args.port, args.baud, args.duration, args.output)
    validate(text, args.duration, required_tasks_mask=RELEASE_REQUIRED_TASKS_MASK)
    final = parse_health(text)[-1]
    if final.get("drops_uplink", 0) or final.get("drops_inbound", 0):
        raise SystemExit("Xiaozhi queue drops were non-zero at acceptance completion")
    print(f"Acceptance serial and health gates passed; log: {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
