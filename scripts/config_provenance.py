#!/usr/bin/env python3
"""Describe firmware configuration without exposing configured values."""

from __future__ import annotations

import hashlib
import re
from dataclasses import dataclass
from pathlib import Path


SENSITIVE_MACROS = frozenset({
    "DEFAULT_WIFI_SSID",
    "DEFAULT_WIFI_PASS",
    "GOOGLE_MAPS_STATIC_API_KEY",
    "MAPS_API_KEY",
    "AI_VOICE_BEARER_TOKEN",
    "AI_MUSIC_STREAM_SOURCES_JSON",
    "YOUTUBE_STREAM_ENDPOINT",
    "YOUTUBE_PROXY_USER",
    "YOUTUBE_PROXY_PASSWORD",
})


@dataclass(frozen=True)
class ConfigProvenance:
    present: bool
    sha256: str | None
    configured_fields: tuple[str, ...]

    @property
    def provisioned(self) -> bool:
        return bool(self.configured_fields)


def configured_macros(text: str) -> set[str]:
    """Return configured sensitive macro names, never their values."""
    configured: set[str] = set()
    definitions = dict(re.findall(
        r'^\s*#\s*define\s+([A-Za-z_][A-Za-z0-9_]*)\s+"([^"]*)"',
        text,
        re.MULTILINE,
    ))
    for name in SENSITIVE_MACROS:
        value = definitions.get(name, "")
        if value and not (name == "AI_MUSIC_STREAM_SOURCES_JSON" and value == "[]"):
            configured.add(name)
    return configured


def inspect_config(path: Path) -> ConfigProvenance:
    """Fingerprint the exact config bytes used by a build."""
    if not path.is_file():
        return ConfigProvenance(False, None, ())
    contents = path.read_bytes()
    text = contents.decode("utf-8")
    return ConfigProvenance(
        True,
        hashlib.sha256(contents).hexdigest(),
        tuple(sorted(configured_macros(text))),
    )
