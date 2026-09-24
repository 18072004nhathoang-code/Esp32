#!/usr/bin/env python3
"""Fail a hardware run when the serial log contains fatal signatures."""

import argparse
import re


FATAL = re.compile(
    r"Guru Meditation|Core \d panic|TASK_WDT|INT_WDT|stack overflow|"
    r"Brownout detector|assert failed|abort\(\)|Backtrace:",
    re.IGNORECASE,
)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("log")
    args = parser.parse_args()
    text = open(args.log, encoding="utf-8", errors="replace").read()
    matches = sorted(set(match.group(0) for match in FATAL.finditer(text)))
    if matches:
        raise SystemExit("fatal serial signatures: " + ", ".join(matches))
    print("Serial log contains no fatal signatures.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
