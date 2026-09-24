#!/usr/bin/env python3
"""Create a hash-identified firmware release bundle from a PlatformIO build."""

from __future__ import annotations

import argparse
import hashlib
import json
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


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--environment", default="esp32-s3-es3c28p")
    parser.add_argument("--output", type=Path, default=Path("release"))
    args = parser.parse_args()
    source = Path(".pio/build") / args.environment
    missing = [name for name in ARTIFACTS if not (source / name).is_file()]
    if missing:
        raise SystemExit(f"missing build artifacts: {', '.join(missing)}")

    args.output.mkdir(parents=True, exist_ok=True)
    revision = subprocess.check_output(
        ["git", "rev-parse", "HEAD"], text=True, stderr=subprocess.DEVNULL
    ).strip()
    source_dirty = bool(subprocess.check_output(
        ["git", "status", "--porcelain", "--untracked-files=normal"],
        text=True,
        stderr=subprocess.DEVNULL,
    ).strip())
    manifest = {
        "environment": args.environment,
        "revision": revision,
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
