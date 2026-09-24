# YouTube Audio Proxy – Backend

Server Node.js tối giản dùng `yt-dlp` để proxy stream âm thanh YouTube về ESP32.

## Endpoints

| Method | Path | Mô tả |
|--------|------|--------|
| `GET` | `/healthz` | Kiểm tra trạng thái server |
| `GET` | `/youtube/stream?q=<từ khóa>` | Proxy stream âm thanh từ YouTube |

## Cài đặt

```bash
npm install
```

## Yêu cầu

- **Node.js** >= 20
- **[yt-dlp](https://github.com/yt-dlp/yt-dlp)** phải được cài sẵn trên hệ thống (hoặc cung cấp đường dẫn qua biến `YT_DLP_PATH`)

## Cấu hình môi trường

```bash
cp .env.example .env
```

| Biến | Mặc định | Mô tả |
|------|----------|--------|
| `HOST` | `127.0.0.1` | Địa chỉ lắng nghe |
| `PORT` | `8787` | Cổng lắng nghe |
| `YT_DLP_PATH` | *(tự dò)* | Đường dẫn tuyệt đối tới `yt-dlp` |
| `PROXY_USER` / `PROXY_PASSWORD` | *(trống)* | Bắt buộc khi bind ra LAN; cấu hình cùng giá trị trong firmware |
| `MAX_STREAMS` | `2` | Giới hạn luồng đồng thời |
| `STREAM_IDLE_TIMEOUT_MS` | `30000` | Hủy upstream không sinh dữ liệu |

Giữ `HOST=127.0.0.1` khi chạy cục bộ. Để ESP32 truy cập, đặt `HOST=0.0.0.0`,
cấu hình credential và chỉ mở cổng trong LAN tin cậy. HTTP Basic không
mã hóa credential; triển khai Internet phải đặt proxy này sau HTTPS reverse proxy.

## Chạy server

```bash
node server.js
```

## Chạy tests

```bash
npm test
```
