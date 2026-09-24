#!/usr/bin/env python3
"""Reject CI firmware configuration that contains deployable credentials."""

from __future__ import annotations

import argparse
import re
from pathlib import Path


SENSITIVE_MACROS = {
    "DEFAULT_WIFI_SSID",
    "DEFAULT_WIFI_PASS",
    "GOOGLE_MAPS_STATIC_API_KEY",
    "MAPS_API_KEY",
    "AI_VOICE_BEARER_TOKEN",
    "AI_MUSIC_STREAM_SOURCES_JSON",
    "YOUTUBE_STREAM_ENDPOINT",
    "YOUTUBE_PROXY_USER",
    "YOUTUBE_PROXY_PASSWORD",
}


def configured_macros(text: str) -> set[str]:
    configured: set[str] = set()
    definitions = dict(re.findall(
        r"^\s*#\s*define\s+([A-Za-z_][A-Za-z0-9_]*)\s+\"([^\"]*)\"",
        text,
        re.MULTILINE,
    ))
    for name in SENSITIVE_MACROS:
        value = definitions.get(name, "")
        if value and not (name == "AI_MUSIC_STREAM_SOURCES_JSON" and value == "[]"):
            configured.add(name)
    return configured


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
