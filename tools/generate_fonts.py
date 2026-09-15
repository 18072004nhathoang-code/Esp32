#!/usr/bin/env python3
"""
tools/generate_fonts.py
Script tái tạo (reproduce) các bộ custom font LVGL 8 cho ESP32-S3 Mini OS.
Sử dụng mã nguồn Font mở (SIL Open Font License 1.1 - Roboto / Montserrat / Noto Sans)
hỗ trợ đầy đủ ASCII và tiếng Việt Unicode (bao gồm U+1EA0 - U+1EF9).

Yêu cầu:
    npm install -g lv_font_conv
    pip install requests (tùy chọn để tải TTF)
"""

import os
import sys
import subprocess

# Các dải ký tự Unicode cần thiết:
# - 0x20-0x7F: ASCII cơ bản
# - 0xA0-0xFF: Latin-1 Supplement (dấu, độ, ký hiệu cơ bản)
# - 0x100-0x17F: Latin Extended-A (Ă, Â, Đ, Ê, Ô, Ơ, Ư...)
# - 0x1A0-0x1B0: Latin Extended-B (Ơ, Ư...)
# - 0x1EA0-0x1EF9: Latin Extended Additional (Dấu tiếng Việt: Ạ, Ả, Ầ, Ẩ, Ỡ, Ợ, Ự...)
# - 0x2010-0x2026: Dấu câu mở rộng (dấu gạch ngang, ba chấm...)
# - 0xB0-0xB7: Ký hiệu độ, dấu chấm giữa
# - 0x20AB: Ký hiệu tiền tệ Đồng Việt Nam ₫
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

def main():
    root_dir = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
    out_dir = os.path.join(root_dir, "src", "ui", "fonts")
    os.makedirs(out_dir, exist_ok=True)

    font_path = sys.argv[1] if len(sys.argv) > 1 else None

    if not font_path or not os.path.exists(font_path):
        print("Usage: python generate_fonts.py <path_to_open_license_font.ttf>")
        print("Ví dụ: python generate_fonts.py Roboto-Regular.ttf")
        print("Mẹo: Bạn có thể tải Roboto / Montserrat / Be Vietnam Pro từ Google Fonts (SIL OFL 1.1).")
        return 1

    print(f"Bắt đầu chuyển đổi font từ: {font_path}")
    print(f"Unicode Ranges: {UNICODE_RANGES}")

    for size in SIZES:
        out_file = os.path.join(out_dir, f"ui_font_{size}.c")
        cmd = [
            "npx", "lv_font_conv",
            "--bpp", "4",
            "--size", str(size),
            "--font", font_path,
            "-r", UNICODE_RANGES,
            "--format", "lvgl",
            "-o", out_file
        ]
        print(f"-> Tạo {out_file} (size {size}px)...")
        res = subprocess.run(cmd, shell=True)
        if res.returncode != 0:
            print(f"Lỗi khi chạy lv_font_conv cho size {size}!")
            return res.returncode

        # Fix guard macro name trong file .c sinh ra để không xung đột macro con trỏ UI_FONT_*
        with open(out_file, "r", encoding="utf-8") as f:
            content = f.read()

        fixed_content = content.replace(f"#ifndef UI_FONT_{size}", f"#ifndef ENABLE_UI_FONT_{size}")
        fixed_content = fixed_content.replace(f"#define UI_FONT_{size} 1", f"#define ENABLE_UI_FONT_{size} 1")

        header_comment = f"""/**
 * @file ui_font_{size}.c
 * @brief Custom LVGL Font size {size}px (ASCII + Full Tiếng Việt Unicode)
 * @license SIL Open Font License (OFL 1.1) / Roboto / Montserrat Font Family
 * Generated with tools/generate_fonts.py
 */
"""
        if "@file" not in fixed_content:
            fixed_content = header_comment + fixed_content

        with open(out_file, "w", encoding="utf-8") as f:
            f.write(fixed_content)

    print("✔ Tái tạo hoàn tất toàn bộ custom font!")
    return 0

if __name__ == "__main__":
    sys.exit(main())
