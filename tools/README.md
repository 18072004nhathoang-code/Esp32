# Font Generation & Reproducibility

Hệ điều hành Mini OS sử dụng các bộ font nhúng C trực tiếp (`src/ui/fonts/ui_font_*.c`) với kích thước 10px, 12px, 14px, 16px.

## Nguồn gốc Font & Bản quyền (License)
* **Font Family**: **Be Vietnam Pro SemiBold** (Google Fonts).
* **Bản quyền**: [SIL Open Font License 1.1 (OFL-1.1)](https://openfontlicense.org/).
* Hoàn toàn tự do sử dụng, chỉnh sửa và phân phối trong các dự án nguồn mở.
* Không phụ thuộc font bản quyền độc quyền của hệ điều hành Windows (`segoeui.ttf`).
* Tích hợp tự động **LVGL Symbol Fallback** (`lv_font_montserrat_10/12/14/16`) để hiển thị hoàn hảo các biểu tượng WiFi, Pin, Loa, Cài đặt, Play, v.v.

## Cách tái tạo (Reproduce) Font
1. Cài đặt Node.js và công cụ chuyển đổi font LVGL:
   ```bash
   npm install -g lv_font_conv
   ```
2. Chạy script tạo font:
   ```bash
   python tools/generate_fonts.py
   ```
   Script sẽ tự động tải file `BeVietnamPro-SemiBold.ttf` từ Google Fonts nếu chưa có, xuất ra các file C trong `src/ui/fonts/ui_font_*.c` với đầy đủ dải Unicode tiếng Việt (có dấu U+1EA0 - U+1EF9), thiết lập cấu trúc fallback symbol tự động và dùng `shell=False` độc lập nền tảng.
