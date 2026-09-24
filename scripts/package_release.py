#!/usr/bin/env python3
"""Create a hash-identified firmware release bundle from a PlatformIO build."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import shutil
import subprocess
from pathlib import Path


ARTIFACTS = ("firmware.bin", "bootloader.bin", "partitions.bin", "firmware.elf", "firmware.map")


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def current_source_revision() -> tuple[str, str, bool]:
    """Return full HEAD, firmware revision string and dirty state used by the build."""
    full_revision = subprocess.check_output(
        ["git", "rev-parse", "HEAD"], text=True, stderr=subprocess.DEVNULL
    ).strip()
    short_revision = subprocess.check_output(
        ["git", "rev-parse", "--short=12", "HEAD"],
        text=True,
        stderr=subprocess.DEVNULL,
    ).strip()
    diff = subprocess.check_output(
        ["git", "diff", "--no-ext-diff", "--binary", "HEAD"],
        stderr=subprocess.DEVNULL,
    )
    untracked = subprocess.check_output(
        ["git", "ls-files", "--others", "--exclude-standard", "-z"],
        stderr=subprocess.DEVNULL,
    ).split(b"\0")
    paths = sorted(path for path in untracked if path)
    dirty = bool(diff or paths)
    firmware_revision = short_revision
    if dirty:
        source_hash = hashlib.sha256(diff)
        for relative_bytes in paths:
            relative = os.fsdecode(relative_bytes)
            source_hash.update(relative_bytes)
            with open(relative, "rb") as source_file:
                source_hash.update(source_file.read())
        firmware_revision += "+wt" + source_hash.hexdigest()[:12]
    return full_revision, firmware_revision, dirty


def validate_build_provenance(metadata: dict[str, object], full_revision: str,
                              firmware_revision: str, source_dirty: bool,
                              allow_dirty: bool = False) -> None:
    if not allow_dirty and source_dirty:
        raise ValueError("refusing to package a dirty working tree")
    if metadata.get("head_revision") != full_revision:
        raise ValueError("build artifacts were produced from a different Git HEAD")
    if metadata.get("firmware_revision") != firmware_revision:
        raise ValueError("build artifacts are stale for the current source state")
    if bool(metadata.get("source_dirty")) != source_dirty:
        raise ValueError("build/source dirty-state metadata does not match")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--environment", default="esp32-s3-es3c28p")
    parser.add_argument("--output", type=Path, default=Path("release"))
    parser.add_argument("--allow-dirty", action="store_true",
                        help="permit a worktree-suffixed developer bundle")
    args = parser.parse_args()
    source = Path(".pio/build") / args.environment
    missing = [name for name in ARTIFACTS if not (source / name).is_file()]
    if missing:
        raise SystemExit(f"missing build artifacts: {', '.join(missing)}")

    metadata_path = source / "source_revision.json"
    if not metadata_path.is_file():
        raise SystemExit("missing build provenance: run pio run before packaging")
    try:
        build_metadata = json.loads(metadata_path.read_text(encoding="utf-8"))
        revision, firmware_revision, source_dirty = current_source_revision()
        validate_build_provenance(build_metadata, revision, firmware_revision,
                                  source_dirty, args.allow_dirty)
    except (json.JSONDecodeError, OSError, subprocess.CalledProcessError, ValueError) as error:
        raise SystemExit(f"build provenance check failed: {error}") from error

    args.output.mkdir(parents=True, exist_ok=True)
    manifest = {
        "environment": args.environment,
        "revision": revision,
        "firmware_revision": firmware_revision,
        "source_dirty": source_dirty,
        "platformio": "6.1.16",
        "platform": "espressif32@6.8.1",
        "dependencies": {
            "ArduinoJson": "6.21.5",
            "ESP32-audioI2S": "928c420d49fce2a09fa91f490b9fcabed6447c67+mini-os.1",
            "LovyanGFX": "1.1.16",
            "TJpg_Decoder": "1.1.0",
            "esp32_opus": "a3816682932b8792f90072ee05c33fe25c055628",
            "lvgl": "8.3.11",
        },
        "artifacts": {},
    }
    for name in ARTIFACTS:
        destination = args.output / name
        shutil.copy2(source / name, destination)
        manifest["artifacts"][name] = {
            "bytes": destination.stat().st_size,
            "sha256": sha256(destination),
        }
    (args.output / "manifest.json").write_text(
        json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    checksums = [f"{entry['sha256']}  {name}"
                 for name, entry in sorted(manifest["artifacts"].items())]
    (args.output / "SHA256SUMS").write_text("\n".join(checksums) + "\n", encoding="utf-8")
    print(f"Packaged {len(ARTIFACTS)} artifacts for {revision} in {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
