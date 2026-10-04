#!/usr/bin/env python3
"""Build and run a host round-trip against PlatformIO's exact pinned Opus checkout."""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import subprocess
import sys


EXPECTED_COMMIT = "a3816682932b8792f90072ee05c33fe25c055628"


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--env", default="esp32-s3-es3c28p")
    parser.add_argument("--cc", default=os.environ.get("CC", "cc"))
    parser.add_argument(
        "--zig-driver", action="store_true",
        help="Treat --cc as the Zig executable and invoke its 'cc' driver",
    )
    args = parser.parse_args()

    root = Path(__file__).resolve().parent.parent
    library = root / ".pio" / "libdeps" / args.env / "esp32_opus"
    source = library / "src"
    if not (source / "opus.h").is_file():
        raise SystemExit(f"Pinned Opus dependency is missing: {source}")

    revision = subprocess.check_output(
        ["git", "-C", str(library), "rev-parse", "HEAD"], text=True
    ).strip()
    if revision != EXPECTED_COMMIT:
        raise SystemExit(
            f"Opus revision mismatch: expected {EXPECTED_COMMIT}, got {revision}"
        )

    config = (source / "config.h").read_text(encoding="utf-8")
    if "#define USE_ALLOCA" not in config or "#define FIXED_POINT" not in config:
        raise SystemExit("Pinned Opus allocation/fixed-point configuration changed")

    sources = sorted(
        path for path in source.glob("*.c") if path.name != "opus_compare.c"
    )
    output_dir = root / ".pio" / "native-tests"
    output_dir.mkdir(parents=True, exist_ok=True)
    executable = output_dir / (
        "xiaozhi_opus_roundtrip.exe" if os.name == "nt"
        else "xiaozhi_opus_roundtrip"
    )
    compiler = [args.cc, "cc"] if args.zig_driver else [args.cc]
    command = [
        *compiler,
        "-std=c99",
        "-O2",
        f"-I{source}",
        *(str(path) for path in sources),
        str(root / "tests" / "xiaozhi_opus_roundtrip_test.c"),
        "-lm",
        "-o",
        str(executable),
    ]
    subprocess.run(command, cwd=root, check=True)
    subprocess.run([str(executable)], cwd=root, check=True)
    print(f"Pinned Opus {revision}: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
