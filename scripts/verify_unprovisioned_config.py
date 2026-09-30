#!/usr/bin/env python3
"""Reject CI firmware configuration that contains deployable credentials."""

from __future__ import annotations

import argparse
from pathlib import Path

from config_provenance import configured_macros


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("config", type=Path)
    args = parser.parse_args()
    if not args.config.is_file():
        raise SystemExit(f"configuration missing: {args.config}")
    configured = sorted(configured_macros(args.config.read_text(encoding="utf-8")))
    if configured:
        raise SystemExit(
            "CI configuration contains provisioned fields: " + ", ".join(configured)
        )
    print("CI firmware configuration is unprovisioned.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
