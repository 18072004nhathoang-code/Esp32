#!/usr/bin/env python3
"""
tools/generate_fonts.py
Script tái tạo (reproduce) các bộ custom font LVGL 8 cho ESP32-S3 Mini OS.
Sử dụng mã nguồn Font mở Be Vietnam Pro SemiBold (SIL Open Font License 1.1)
hỗ trợ đầy đủ ASCII, Latin Extended và toàn bộ tiếng Việt Unicode có dấu (U+1EA0 - U+1EF9),
tự động tích hợp Fallback cho các biểu tượng LVGL (lv_font_montserrat_*).

Yêu cầu:
    Node.js / npm (npx lv_font_conv)
    Python 3
"""

import os
import sys
import shutil
import subprocess
import hashlib

if sys.platform == "win32":
    try:
        sys.stdout.reconfigure(encoding="utf-8")
        sys.stderr.reconfigure(encoding="utf-8")
    except Exception:
        pass

UNICODE_RANGES = (
    "0x20-0x7F,"
    "0xA0-0xFF,"
    "0x100-0x17F,"
    "0x1A0-0x1B0,"
    "0x1EA0-0x1EF9,"
    "0x2010-0x2026,"
    "0xB0-0xB7,"
    "0x20AB"
)

SIZES = [10, 12, 14, 16]

FONT_SHA256 = "bd8e27eb02720b9d91e59e4f10a90878643219f25ce6a8d9a4f06a8a88d3bb71"
LV_FONT_CONV_VERSION = "1.5.3"

def main():
    script_dir = os.path.dirname(os.path.abspath(__file__))
    root_dir = os.path.abspath(os.path.join(script_dir, ".."))
    out_dir = os.path.join(root_dir, "src", "ui", "fonts")
    os.makedirs(out_dir, exist_ok=True)

    font_path = sys.argv[1] if len(sys.argv) > 1 else os.path.join(script_dir, "BeVietnamPro-SemiBold.ttf")

    if not os.path.exists(font_path):
        print(f"Không tìm thấy font tại: {font_path}")
        print("Không tự tải nguồn mutable; dùng file font đã pin trong tools/.")
        return 1

    with open(font_path, "rb") as font_file:
        actual_hash = hashlib.sha256(font_file.read()).hexdigest()
    if actual_hash != FONT_SHA256:
        print(f"❌ SHA256 font không khớp: {actual_hash}")
        return 1

    npx_bin = shutil.which("npx")
    if not npx_bin:
        print("❌ Không tìm thấy lệnh 'npx'. Vui lòng cài đặt Node.js.")
        return 1

    print(f"Bắt đầu chuyển đổi font từ: {font_path}")
    print(f"Unicode Ranges: {UNICODE_RANGES}")

    for size in SIZES:
        out_file = os.path.join(out_dir, f"ui_font_{size}.c")
        cmd = [
            npx_bin, "--yes", f"lv_font_conv@{LV_FONT_CONV_VERSION}",
            "--bpp", "4",
            "--size", str(size),
            "--font", font_path,
            "-r", UNICODE_RANGES,
            "--format", "lvgl",
            "-o", out_file
        ]
        print(f"-> Tạo {out_file} (size {size}px, shell=False)...")
        res = subprocess.run(cmd, shell=False)
        if res.returncode != 0:
            print(f"❌ Lỗi khi chạy lv_font_conv cho size {size}!")
            return res.returncode

        with open(out_file, "r", encoding="utf-8") as f:
            content = f.read()

        # Đổi guard macro để không xung đột macro con trỏ UI_FONT_*
        content = content.replace(f"#ifndef UI_FONT_{size}", f"#ifndef ENABLE_UI_FONT_{size}")
        content = content.replace(f"#define UI_FONT_{size} 1", f"#define ENABLE_UI_FONT_{size} 1")
        content = content.replace(f"#if UI_FONT_{size}", f"#if ENABLE_UI_FONT_{size}")

        # Chuẩn hóa include lvgl.h
        old_inc = """#ifdef LV_LVGL_H_INCLUDE_SIMPLE
#include "lvgl.h"
#else
#include "lvgl/lvgl.h"
#endif"""
        if old_inc in content:
            content = content.replace(old_inc, "#include <lvgl.h>")
        else:
            content = content.replace('#include "lvgl/lvgl.h"', "#include <lvgl.h>")
            content = content.replace('#include "lvgl.h"', "#include <lvgl.h>")

        # Thêm khai báo fallback cho ký hiệu biểu tượng LVGL Symbols sau khi đã include lvgl.h
        fallback_sym = f"lv_font_montserrat_{size}"
        decl = f"LV_FONT_DECLARE({fallback_sym});\n"
        # Xóa decl nếu bị đặt ở đầu file từ trước
        content = content.replace(decl, "")
        content = content.replace("#include <lvgl.h>", f"#include <lvgl.h>\n{decl}")

        # Gán fallback vào cấu trúc lv_font_t
        content = content.replace(".fallback = NULL,", f".fallback = &{fallback_sym},")

        header_comment = f"""/**
 * @file ui_font_{size}.c
 * @brief Custom LVGL Font size {size}px (SemiBold: ASCII + Full Tiếng Việt Unicode + Fallback LVGL Symbols)
 * @license SIL Open Font License (OFL 1.1) / Be Vietnam Pro
 * Generated with tools/generate_fonts.py
 */
"""
        if "@file" not in content:
            content = header_comment + content

        with open(out_file, "w", encoding="utf-8") as f:
            f.write(content)

    print("✔ Tái tạo hoàn tất toàn bộ custom font chất lượng cao!")
    return 0

if __name__ == "__main__":
    sys.exit(main())
