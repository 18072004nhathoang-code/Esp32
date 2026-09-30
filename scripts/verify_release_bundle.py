#!/usr/bin/env python3
"""Independently verify a packaged firmware release directory."""

from __future__ import annotations

import argparse
import json
from pathlib import Path

from package_release import BOOT_APP0_SHA256, FLASH_LAYOUT, sha256


REQUIRED_FILES = {
    "firmware.bin",
    "bootloader.bin",
    "boot_app0.bin",
    "partitions.bin",
    "firmware.elf",
    "firmware.map",
    "FLASHING.md",
    "SOURCE.md",
    "THIRD_PARTY_NOTICES.md",
    "licenses/project-MIT.txt",
    "licenses/ESP32-audioI2S-GPL-3.0.txt",
    "licenses/ESP32-audioI2S-UPSTREAM.md",
    "docs/PRODUCTION_SECURITY.md",
    "docs/RELEASE_CHECKLIST.md",
}


def verify_bundle(bundle: Path, allow_provisioned: bool = False) -> None:
    manifest_path = bundle / "manifest.json"
    sums_path = bundle / "SHA256SUMS"
    if not manifest_path.is_file() or not sums_path.is_file():
        raise ValueError("manifest.json or SHA256SUMS is missing")
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    files = manifest.get("files")
    if not isinstance(files, dict):
        raise ValueError("manifest file inventory is missing")
    missing = sorted(REQUIRED_FILES - set(files))
    if missing:
        raise ValueError("required release files are missing: " + ", ".join(missing))

    expected_flash = {
        "chip": "esp32s3",
        "mode": "qio",
        "frequency": "80m",
        "size": "16MB",
        "layout": [{"offset": offset, "file": name} for offset, name in FLASH_LAYOUT],
    }
    if manifest.get("flash") != expected_flash:
        raise ValueError("flash layout does not match the ESP32-S3 factory layout")
    configuration = manifest.get("configuration", {})
    if not isinstance(configuration, dict):
        raise ValueError("configuration provenance is missing")
    if configuration.get("provisioned") and not allow_provisioned:
        raise ValueError("public release bundle contains provisioned configuration")

    actual_files = {
        path.relative_to(bundle).as_posix()
        for path in bundle.rglob("*")
        if path.is_file() and path.name not in {"manifest.json", "SHA256SUMS"}
    }
    if actual_files != set(files):
        unlisted = sorted(actual_files - set(files))
        absent = sorted(set(files) - actual_files)
        detail = []
        if unlisted:
            detail.append("unlisted=" + ",".join(unlisted))
        if absent:
            detail.append("absent=" + ",".join(absent))
        raise ValueError("bundle inventory mismatch: " + " ".join(detail))

    expected_sums: list[str] = []
    for name, entry in sorted(files.items()):
        path = bundle / name
        if not path.is_file():
            raise ValueError(f"manifest file is missing: {name}")
        if path.stat().st_size != entry.get("bytes") or sha256(path) != entry.get("sha256"):
            raise ValueError(f"size or checksum mismatch: {name}")
        expected_sums.append(f"{entry['sha256']}  {name}")
    if sha256(bundle / "boot_app0.bin") != BOOT_APP0_SHA256:
        raise ValueError("boot_app0.bin checksum is not the pinned framework image")
    actual_sums = sums_path.read_text(encoding="utf-8").splitlines()
    if actual_sums != expected_sums:
        raise ValueError("SHA256SUMS does not exactly match the manifest inventory")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("bundle", type=Path)
    parser.add_argument("--allow-provisioned", action="store_true")
    args = parser.parse_args()
    try:
        verify_bundle(args.bundle, args.allow_provisioned)
    except (OSError, json.JSONDecodeError, ValueError) as error:
        raise SystemExit(f"release bundle verification failed: {error}") from error
    print(f"Release bundle verification passed: {args.bundle}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
