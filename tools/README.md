# Font Generation & Reproducibility

Hệ điều hành Mini OS sử dụng các bộ font nhúng C trực tiếp (`src/ui/fonts/ui_font_*.c`) với kích thước 10px, 12px, 14px, 16px.

## Nguồn gốc Font & Bản quyền (License)
* **Font Family**: Google Fonts (Roboto / Montserrat / Be Vietnam Pro).
* **Bản quyền**: [SIL Open Font License 1.1 (OFL-1.1)](https://openfontlicense.org/).
* Hoàn toàn tự do sử dụng, chỉnh sửa và phân phối trong các dự án nguồn mở (MIT / Apache / BSD).
* Không phụ thuộc font bản quyền độc quyền của hệ điều hành Windows (`segoeui.ttf`).

## Cách tái tạo (Reproduce) Font
1. Cài đặt công cụ chuyển đổi font LVGL:
   ```bash
   npm install -g lv_font_conv
   ```
2. Chạy script tạo font:
   ```bash
   python tools/generate_fonts.py <path_to_open_font.ttf>
   ```
   Script sẽ tự động xuất ra các file `src/ui/fonts/ui_font_10.c`, `12.c`, `14.c`, `16.c` và sửa các macro guard để tương thích với abstraction `UI_FONT_10`, `UI_FONT_12`, `UI_FONT_14`, `UI_FONT_16`.
