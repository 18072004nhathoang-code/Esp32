#!/usr/bin/env python3
"""Exercise production BLE layout with pinned LVGL and the firmware fonts."""
import argparse
import json
from pathlib import Path
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--compiler", nargs="+", default=["g++"])
    parser.add_argument("--sanitizer", default="address,undefined")
    args = parser.parse_args()
    root = Path(__file__).resolve().parent.parent
    lvgl = root / ".pio/libdeps/esp32-s3-es3c28p/lvgl"
    if json.loads((lvgl / "library.json").read_text(encoding="utf-8"))["version"] != "8.3.11":
        raise SystemExit("Expected pinned LVGL 8.3.11; run pio pkg install first")
    flags = ["-DLV_CONF_SKIP=1", "-DLV_COLOR_DEPTH=16", "-DLV_COLOR_16_SWAP=0",
             "-DLV_MEM_SIZE=98304", "-DLV_FONT_MONTSERRAT_12=1",
             "-DLV_FONT_MONTSERRAT_14=1", "-DLV_FONT_MONTSERRAT_16=1",
             "-I" + (root / "include").as_posix(), "-I" + lvgl.as_posix(),
             "-I" + (root / "src/ui").as_posix(),
             "-fsanitize=" + args.sanitizer, "-fno-sanitize-recover=all"]
    with tempfile.TemporaryDirectory(prefix="esp32-lvgl-test-") as temporary:
        build = Path(temporary)
        obj = build / "layout.o"
        subprocess.run(args.compiler + flags + ["-std=c++11", "-Wall", "-Wextra", "-Werror",
                       "-c", (root / "tests/ble_settings_layout_test.cpp").as_posix(),
                       "-o", obj.as_posix()], check=True, cwd=root, timeout=120)
        sources = sorted(lvgl.joinpath("src").rglob("*.c"))
        sources += [root / "src/ui/fonts/ui_font_12.c", root / "src/ui/fonts/ui_font_14.c",
                    root / "src/ui/fonts/ui_font_16.c"]
        executable = build / "ble_settings_layout.exe"
        # A response file avoids Windows command-line limits. Only temporary
        # build artifacts are written; tests never mutate dependency/source files.
        response = build / "lvgl.rsp"
        arguments = flags + ["-x", "c", "-std=c99"] + [p.as_posix() for p in sources]
        arguments += ["-x", "none", obj.as_posix(), "-o", executable.as_posix()]
        response.write_text("\n".join(json.dumps(a, ensure_ascii=False) for a in arguments),
                            encoding="utf-8")
        subprocess.run(args.compiler + ["@" + response.as_posix()], check=True, cwd=root, timeout=120)
        subprocess.run([str(executable)], check=True, cwd=root, timeout=15)
        # Same production constructors as firmware, not a geometry model.
        subprocess.run(args.compiler + flags + ["-std=c++11", "-Wall", "-Wextra", "-Werror",
                       "-c", (root / "tests/ui_shell_layout_test.cpp").as_posix(),
                       "-o", obj.as_posix()], check=True, cwd=root, timeout=120)
        subprocess.run(args.compiler + ["@" + response.as_posix()], check=True, cwd=root, timeout=120)
        subprocess.run([str(executable)], check=True, cwd=root, timeout=15)


if __name__ == "__main__":
    main()
