#!/usr/bin/env python3
"""Run and validate the dedicated 30-minute SD music stress firmware."""

import argparse
import re
from pathlib import Path

from hil_common import capture, validate


MUSIC_STRESS_TASK_MASK = 1 << 13
COMPLETE = re.compile(
    r"\[HW_STRESS\] COMPLETE heap=(\d+) delta=(-?\d+) tasks=(\d+) delta=(-?\d+)"
)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", required=True)
    parser.add_argument("--duration", type=int, default=1840)
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--output", type=Path, default=Path("hil-music-stress.log"))
    args = parser.parse_args()

    text = capture(args.port, args.baud, args.duration, args.output)
    validate(text, args.duration, required_tasks_mask=MUSIC_STRESS_TASK_MASK)
    if "[HW_STRESS] FAIL" in text:
        raise SystemExit("music stress firmware reported a failure")
    complete = COMPLETE.search(text)
    if not complete:
        raise SystemExit("music stress did not reach COMPLETE")
    heap_delta = int(complete.group(2))
    task_delta = int(complete.group(4))
    if heap_delta < -8192:
        raise SystemExit(f"music stress internal heap drift is {heap_delta} bytes")
    if task_delta != 0:
        raise SystemExit(f"music stress task-count drift is {task_delta}")
    print(f"30-minute SD music stress passed; log: {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
