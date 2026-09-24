#!/usr/bin/env python3
"""Validate the fixed 16 MB ES3C28P partition contract."""

import argparse
import csv


EXPECTED = {
    "nvs": (0x9000, 0x5000),
    "otadata": (0xE000, 0x2000),
    "app0": (0x10000, 0x480000),
    "app1": (0x490000, 0x480000),
    "spiffs": (0x910000, 0x6E0000),
    "coredump": (0xFF0000, 0x10000),
}
FLASH_SIZE = 16 * 1024 * 1024


def number(value: str) -> int:
    return int(value.strip(), 0)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("path")
    args = parser.parse_args()
    rows = []
    with open(args.path, newline="", encoding="utf-8") as source:
        for raw in csv.reader(line for line in source if not line.lstrip().startswith("#")):
            if not raw or not raw[0].strip():
                continue
            name = raw[0].strip()
            rows.append((name, number(raw[3]), number(raw[4])))

    actual = {name: (offset, size) for name, offset, size in rows}
    if actual != EXPECTED:
        raise SystemExit(f"partition contract mismatch: {actual!r}")
    ordered = sorted(rows, key=lambda row: row[1])
    for previous, current in zip(ordered, ordered[1:]):
        if previous[1] + previous[2] > current[1]:
            raise SystemExit(f"overlap: {previous[0]} and {current[0]}")
    if ordered[-1][1] + ordered[-1][2] != FLASH_SIZE:
        raise SystemExit("partition table does not end at 16 MB")
    print("Partition table valid: 16 MB, non-overlapping, fixed offsets.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
