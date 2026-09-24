#!/usr/bin/env python3
"""Enforce byte budgets from the RAM/Flash summary emitted by `pio run`."""

import argparse
import re


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("log")
    parser.add_argument("--max-ram", type=int, required=True)
    parser.add_argument("--max-flash", type=int, required=True)
    args = parser.parse_args()
    text = open(args.log, encoding="utf-8", errors="replace").read()
    ram = re.search(r"RAM:\s+\[[^]]+\]\s+[\d.]+% \(used (\d+) bytes", text)
    flash = re.search(r"Flash:\s+\[[^]]+\]\s+[\d.]+% \(used (\d+) bytes", text)
    if not ram or not flash:
        raise SystemExit("could not parse PlatformIO size output")
    ram_bytes, flash_bytes = int(ram.group(1)), int(flash.group(1))
    if ram_bytes > args.max_ram or flash_bytes > args.max_flash:
        raise SystemExit(
            f"size budget exceeded: RAM {ram_bytes}/{args.max_ram}, "
            f"flash {flash_bytes}/{args.max_flash}"
        )
    print(f"Size budgets pass: RAM {ram_bytes}, flash {flash_bytes}.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
