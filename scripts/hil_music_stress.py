#!/usr/bin/env python3
"""Run and validate the dedicated 30-minute SD music stress firmware."""

import argparse
import re
from pathlib import Path

from hil_common import capture, current_git_revision, validate


MUSIC_STRESS_TASK_MASK = 1 << 13
COMPLETE = re.compile(
    r"\[HW_STRESS\] COMPLETE heap=(\d+) delta=(-?\d+) tasks=(\d+) delta=(-?\d+)"
)
DECODER_STACK = re.compile(r"PeriodicTask high-water=(\d+) bytes")
MUSIC_FATAL = re.compile(
    r"register I2S object to platform failed|duplex restore failed|"
    r"ownership retained for retry",
    re.IGNORECASE,
)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", required=True)
    parser.add_argument("--duration", type=int, default=1840)
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--output", type=Path, default=Path("hil-music-stress.log"))
    parser.add_argument("--revision", default="")
    args = parser.parse_args()

    text = capture(args.port, args.baud, args.duration, args.output)
    validate(text, args.duration, required_tasks_mask=MUSIC_STRESS_TASK_MASK,
             expected_revision=args.revision or current_git_revision(),
             require_fresh_boot=True, expect_music_stress=True)
    if "[HW_STRESS] FAIL" in text:
        raise SystemExit("music stress firmware reported a failure")
    music_fatal = MUSIC_FATAL.search(text)
    if music_fatal:
        raise SystemExit(f"music/I2S failure signature: {music_fatal.group(0)}")
    decoder_margins = [int(value) for value in DECODER_STACK.findall(text)]
    if not decoder_margins:
        raise SystemExit("decoder task stack margin was not reported")
    if min(decoder_margins) < 2048:
        raise SystemExit(
            f"decoder task stack margin is {min(decoder_margins)} bytes; required >= 2048"
        )
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
