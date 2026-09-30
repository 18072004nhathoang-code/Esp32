#!/usr/bin/env python3
"""Create a complete, hash-identified firmware release bundle."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import shutil
import subprocess
from pathlib import Path

from config_provenance import ConfigProvenance, inspect_config


BUILD_ARTIFACTS = (
    "firmware.bin",
    "bootloader.bin",
    "partitions.bin",
    "firmware.elf",
    "firmware.map",
)
BOOT_APP0_SHA256 = "f94c5d786a7a8fab06ac5d10e33bf37711a6697636dc037559ea19cc410a17f0"
SOURCE_REPOSITORY = "https://github.com/18072004nhathoang-code/Esp32"
FLASH_LAYOUT = (
    ("0x0000", "bootloader.bin"),
    ("0x8000", "partitions.bin"),
    ("0xe000", "boot_app0.bin"),
    ("0x10000", "firmware.bin"),
)
SUPPORT_FILES = {
    "LICENSE": "licenses/project-MIT.txt",
    "THIRD_PARTY_NOTICES.md": "THIRD_PARTY_NOTICES.md",
    "LICENSES/OFL-1.1.txt": "licenses/OFL-1.1.txt",
    "lib/ESP32-audioI2S-patched/LICENSE": "licenses/ESP32-audioI2S-GPL-3.0.txt",
    "lib/ESP32-audioI2S-patched/UPSTREAM.md": "licenses/ESP32-audioI2S-UPSTREAM.md",
    "docs/PRODUCTION_SECURITY.md": "docs/PRODUCTION_SECURITY.md",
    "docs/RELEASE_CHECKLIST.md": "docs/RELEASE_CHECKLIST.md",
    "platformio.ini": "source/platformio.ini",
    "partitions.csv": "source/partitions.csv",
    "boards/esp32-s3-devkitc-1-n16r8.json": "source/esp32-s3-devkitc-1-n16r8.json",
    "include/secrets.example.h": "source/secrets.example.h",
}


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
                              config: ConfigProvenance | None = None,
                              allow_dirty: bool = False,
                              allow_provisioned: bool = False) -> None:
    if not allow_dirty and source_dirty:
        raise ValueError("refusing to package a dirty working tree")
    if metadata.get("head_revision") != full_revision:
        raise ValueError("build artifacts were produced from a different Git HEAD")
    if metadata.get("firmware_revision") != firmware_revision:
        raise ValueError("build artifacts are stale for the current source state")
    if bool(metadata.get("source_dirty")) != source_dirty:
        raise ValueError("build/source dirty-state metadata does not match")
    if config is None:
        return
    if metadata.get("config_present") != config.present:
        raise ValueError("build configuration presence does not match")
    if metadata.get("config_sha256") != config.sha256:
        raise ValueError("build artifacts are stale for the current firmware configuration")
    if tuple(metadata.get("config_fields", ())) != config.configured_fields:
        raise ValueError("build configuration field metadata does not match")
    if bool(metadata.get("config_provisioned")) != config.provisioned:
        raise ValueError("build configuration provisioning metadata does not match")
    if config.provisioned and not allow_provisioned:
        fields = ", ".join(config.configured_fields)
        raise ValueError(
            "refusing to package provisioned firmware; configured fields: " + fields
        )


def locate_boot_app0(explicit: Path | None = None) -> Path:
    candidates: list[Path] = []
    if explicit is not None:
        candidates.append(explicit)
    core_dir = os.environ.get("PLATFORMIO_CORE_DIR")
    if core_dir:
        candidates.append(Path(core_dir) / "packages" / "framework-arduinoespressif32" /
                          "tools" / "partitions" / "boot_app0.bin")
    candidates.append(Path.home() / ".platformio" / "packages" /
                      "framework-arduinoespressif32" / "tools" / "partitions" /
                      "boot_app0.bin")
    for candidate in candidates:
        if candidate.is_file():
            if sha256(candidate) != BOOT_APP0_SHA256:
                raise ValueError(
                    "boot_app0.bin does not match pinned Arduino-ESP32 2.0.17"
                )
            return candidate
    raise FileNotFoundError(
        "boot_app0.bin not found in the installed Arduino-ESP32 framework package"
    )


def flashing_instructions(config: ConfigProvenance) -> str:
    configuration_note = (
        "This is a private provisioned image containing deployment configuration. "
        "Do not distribute or upload this bundle."
        if config.provisioned else
        "This public image is unprovisioned and starts without Wi-Fi/API credentials. "
        "Configure the device locally after flashing."
    )
    return f"""# Flashing the ESP32-S3 release

Target: ES3C28P N16R8 (ESP32-S3, 16 MB flash, 8 MB OPI PSRAM).

Install esptool, set the serial port, then write every factory image:

```sh
python3 -m pip install esptool
export ESP32_PORT=/dev/cu.usbmodemXXXX
python3 -m esptool --chip esp32s3 --port "$ESP32_PORT" --baud 460800 \\
  write_flash --flash_mode qio --flash_freq 80m --flash_size 16MB \\
  0x0000 bootloader.bin 0x8000 partitions.bin \\
  0xe000 boot_app0.bin 0x10000 firmware.bin
```

{configuration_note} Do not flash it onto a different board model.
"""


def source_instructions(revision: str, dirty: bool, config: ConfigProvenance) -> str:
    fields = ", ".join(config.configured_fields) if config.configured_fields else "none"
    source_note = (
        "This is a private developer bundle from a modified worktree; the linked commit "
        "does not include those uncommitted changes and this bundle must not be distributed."
        if dirty else
        "The exact tracked Corresponding Source is available at the immutable commit below."
    )
    return f"""# Corresponding Source and build information

{source_note}

- Repository: {SOURCE_REPOSITORY}
- Commit: [{revision}]({SOURCE_REPOSITORY}/tree/{revision})
- PlatformIO Core: 6.1.16
- Platform: espressif32@6.8.1 (Arduino-ESP32 2.0.17)
- Environment: esp32-s3-es3c28p
- Provisioned configuration: {str(config.provisioned).lower()}
- Configured field names: {fields}

Build from the repository root with:

```sh
cp include/secrets.example.h include/secrets.h
pio run -e esp32-s3-es3c28p
```

Private credential values are never included in this bundle. See
`THIRD_PARTY_NOTICES.md` for GPL and other third-party source obligations.
"""


def prepare_output(path: Path, replace: bool) -> None:
    if path.exists() and any(path.iterdir()):
        if not replace:
            raise ValueError("output directory is not empty; use --replace intentionally")
        shutil.rmtree(path)
    path.mkdir(parents=True, exist_ok=True)


def copy_file(source: Path, destination: Path) -> None:
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(source, destination)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--environment", default="esp32-s3-es3c28p")
    parser.add_argument("--output", type=Path, default=Path("release"))
    parser.add_argument("--boot-app0", type=Path,
                        help="explicit framework boot_app0.bin path")
    parser.add_argument("--allow-dirty", action="store_true",
                        help="permit a worktree-suffixed developer bundle")
    parser.add_argument("--allow-provisioned", action="store_true",
                        help="permit a private bundle containing configured credentials")
    parser.add_argument("--replace", action="store_true",
                        help="replace a non-empty output directory")
    args = parser.parse_args()
    source = Path(".pio/build") / args.environment
    missing = [name for name in BUILD_ARTIFACTS if not (source / name).is_file()]
    if missing:
        raise SystemExit(f"missing build artifacts: {', '.join(missing)}")

    metadata_path = source / "source_revision.json"
    if not metadata_path.is_file():
        raise SystemExit("missing build provenance: run pio run before packaging")
    try:
        build_metadata = json.loads(metadata_path.read_text(encoding="utf-8"))
        revision, firmware_revision, source_dirty = current_source_revision()
        config = inspect_config(Path("include/secrets.h"))
        validate_build_provenance(
            build_metadata, revision, firmware_revision, source_dirty, config,
            args.allow_dirty, args.allow_provisioned,
        )
        boot_app0 = locate_boot_app0(args.boot_app0)
        prepare_output(args.output, args.replace)
    except (json.JSONDecodeError, OSError, subprocess.CalledProcessError, ValueError) as error:
        raise SystemExit(f"release packaging failed: {error}") from error

    manifest: dict[str, object] = {
        "environment": args.environment,
        "revision": revision,
        "firmware_revision": firmware_revision,
        "source_dirty": source_dirty,
        "configuration": {
            "provisioned": config.provisioned,
            "configured_fields": list(config.configured_fields),
        },
        "platformio": "6.1.16",
        "platform": "espressif32@6.8.1",
        "flash": {
            "chip": "esp32s3",
            "mode": "qio",
            "frequency": "80m",
            "size": "16MB",
            "layout": [{"offset": offset, "file": name} for offset, name in FLASH_LAYOUT],
        },
        "dependencies": {
            "ArduinoJson": "6.21.5",
            "ESP32-audioI2S": "928c420d49fce2a09fa91f490b9fcabed6447c67+mini-os.1",
            "LovyanGFX": "1.1.16",
            "TJpg_Decoder": "1.1.0",
            "esp32_opus": "a3816682932b8792f90072ee05c33fe25c055628",
            "lvgl": "8.3.11",
        },
        "files": {},
    }
    for name in BUILD_ARTIFACTS:
        copy_file(source / name, args.output / name)
    copy_file(boot_app0, args.output / "boot_app0.bin")
    for source_name, destination_name in SUPPORT_FILES.items():
        copy_file(Path(source_name), args.output / destination_name)
    (args.output / "FLASHING.md").write_text(
        flashing_instructions(config), encoding="utf-8"
    )
    (args.output / "SOURCE.md").write_text(
        source_instructions(revision, source_dirty, config), encoding="utf-8"
    )

    excluded = {"manifest.json", "SHA256SUMS"}
    bundle_files = sorted(
        path for path in args.output.rglob("*")
        if path.is_file() and path.relative_to(args.output).as_posix() not in excluded
    )
    files_manifest = manifest["files"]
    assert isinstance(files_manifest, dict)
    for path in bundle_files:
        name = path.relative_to(args.output).as_posix()
        files_manifest[name] = {"bytes": path.stat().st_size, "sha256": sha256(path)}

    (args.output / "manifest.json").write_text(
        json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    checksums = [f"{entry['sha256']}  {name}"
                 for name, entry in sorted(files_manifest.items())]
    (args.output / "SHA256SUMS").write_text("\n".join(checksums) + "\n", encoding="utf-8")
    print(f"Packaged {len(bundle_files)} verified files for {revision} in {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
