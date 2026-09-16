"""
========================================================
 ESP32-S3 Factory Reset Binary Generator
 Board: ESP32-S3 N16R8 (16MB Flash)
 Partition Table: partitions.csv
========================================================

Generates blank .bin files (filled with 0xFF) for:
  - NVS partition    : offset 0x9000,  size 0x5000  (20 KB)
  - OTA Data         : offset 0xE000,  size 0x2000  (8 KB)
  - SPIFFS (optional): offset 0x910000,size 0x6E0000 (7 MB)

Flash command example (esptool):
  esptool.py --chip esp32s3 --port COMx write_flash \
      0x9000  nvs_blank.bin \
      0xE000  otadata_blank.bin
========================================================
"""

import os
import struct

# ── Partition layout (from partitions.csv) ─────────────────────────────────
PARTITIONS = {
    "nvs":      {"offset": 0x9000,   "size": 0x5000},    # 20 KB
    "otadata":  {"offset": 0xE000,   "size": 0x2000},    # 8 KB
    "spiffs":   {"offset": 0x910000, "size": 0x6E0000},  # 7 MB (optional)
}

OUTPUT_DIR = os.path.dirname(os.path.abspath(__file__))


def generate_blank_bin(name: str, size: int) -> str:
    """Tạo file .bin toàn 0xFF với kích thước 'size' bytes."""
    filename = os.path.join(OUTPUT_DIR, f"{name}_blank.bin")
    with open(filename, "wb") as f:
        f.write(b"\xFF" * size)
    print(f"[OK] {filename}  ({size} bytes = {size // 1024} KB)")
    return filename


def main():
    print("=" * 60)
    print("  ESP32-S3 Factory Reset - Blank Binary Generator")
    print("=" * 60)

    # ── Bắt buộc: NVS + OTA Data ──────────────────────────────────────────
    generate_blank_bin("nvs",     PARTITIONS["nvs"]["size"])
    generate_blank_bin("otadata", PARTITIONS["otadata"]["size"])

    # ── Tuỳ chọn: SPIFFS (cảnh báo: file rất lớn ~7 MB) ──────────────────
    ans = input("\nTạo spiffs_blank.bin (~7 MB)? [y/N]: ").strip().lower()
    if ans == "y":
        generate_blank_bin("spiffs", PARTITIONS["spiffs"]["size"])
    else:
        print("[SKIP] spiffs_blank.bin")

    print("\n" + "=" * 60)
    print("  Hoàn tất! Xem file flash_reset.bat để nạp vào board.")
    print("=" * 60)


if __name__ == "__main__":
    main()
