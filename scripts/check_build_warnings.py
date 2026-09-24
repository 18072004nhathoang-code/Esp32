#!/usr/bin/env python3
"""Reject project warnings and dependency warnings outside the audited allowlist."""

from __future__ import annotations

import argparse
from pathlib import Path


def allowed(line: str) -> bool:
    normalized = line.replace("\\", "/")
    return (
        ".pio/libdeps/" in normalized
        and "LovyanGFX/" in normalized
        and 'warning: "REG_SPI_BASE" redefined' in normalized
    ) or (
        ".pio/libdeps/" in normalized
        and "esp32_opus/" in normalized
        and "warning: #warning" in normalized
        and ("lrint()" in normalized or "standard C cast" in normalized)
    )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("log", type=Path)
    args = parser.parse_args()
    warnings = [line for line in args.log.read_text(errors="replace").splitlines()
                if ": warning:" in line]
    unexpected = [line for line in warnings if not allowed(line)]
    if unexpected:
        print("Unclassified compiler warnings:")
        print("\n".join(unexpected))
        return 1
    print(f"Build warning gate passed ({len(warnings)} audited third-party warnings).")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
