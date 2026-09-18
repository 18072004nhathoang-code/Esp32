# ESP32 Mini OS AI backend

Backend cho app **AI Voice** hiện có. Thiết bị gửi WAV PCM16 mono 16 kHz tới `POST /v1/query`; backend dùng Gemini để chép lời/trả lời tiếng Việt, bật Google Search grounding và trả nguồn. `POST /v1/tts` chuyển văn bản thành WAV PCM16 mono 16 kHz.

## Chạy

Yêu cầu Node.js 20 trở lên và reverse proxy HTTPS có chứng chỉ hợp lệ.

```bash
cd backend
cp .env.example .env
npm test
npm start
```

Điền `GEMINI_API_KEY` chỉ trong `backend/.env`. Tạo `ESP_DEVICE_TOKEN` độc lập, ngẫu nhiên tối thiểu 32 ký tự. Backend mặc định bind `127.0.0.1:8787`; public Internet chỉ đi qua reverse proxy HTTPS. Không đưa Gemini key vào firmware.

Ví dụ Caddy:

```caddyfile
assistant.example.com {
    reverse_proxy 127.0.0.1:8787
    request_body {
        max_size 1MB
    }
}
```

Sao chép CA PEM của chuỗi chứng chỉ HTTPS vào `AI_VOICE_CA_CERT`, đặt URL `/v1/query`, `/v1/tts` và cùng `ESP_DEVICE_TOKEN` trong `include/secrets.h`. Firmware fail-closed nếu URL không phải HTTPS, token hoặc CA trống.

## Nguồn nhạc

Khai báo cùng ID ở hai nơi. Backend chỉ đưa ID đã cấu hình vào action; firmware tự ánh xạ ID sang URL HTTPS cục bộ.

`backend/.env`:

```dotenv
MUSIC_SOURCES_JSON=[{"id":"radio1","label":"Radio 1","url":"https://radio.example/live.mp3"}]
```

`include/secrets.h`:

```c
#define AI_MUSIC_STREAM_SOURCES_JSON "[{\"id\":\"radio1\",\"url\":\"https://radio.example/live.mp3\"}]"
#define AI_MUSIC_STREAM_CA_CERT "-----BEGIN CERTIFICATE-----\n...\n-----END CERTIFICATE-----\n"
```

Chỉ URL HTTPS được chấp nhận. Nếu ID thiếu/sai, queue đầy, decoder không mở được stream hoặc I2S không được cấp, thiết bị báo thất bại. Không có source cấu hình thì lệnh phát dùng bài hiện tại/đầu tiên trên SD; nếu SD không có MP3 thì trả lỗi thật.

## Giao thức và giới hạn

- `POST /v1/query`, `Content-Type: audio/wav`, bearer token: tối đa 400.000 byte mặc định; WAV phải PCM16 mono 16 kHz.
- Phản hồi: `{"transcript":"...","reply":"...","actions":[],"sources":[]}`; mỗi chuỗi chat tối đa 511 byte UTF-8, tối đa 2 action và 3 nguồn.
- Action duy nhất: `music.play`, `music.pause`, `music.resume`, `music.stop`, `music.volume` (0–100).
- `POST /v1/tts`, JSON đúng một trường `text`: trả RIFF/WAV PCM16 mono 16 kHz.
- Deadline mặc định 45 giây. Body quá cỡ, token sai, WAV lỗi, Gemini timeout và câu hỏi thời sự không có Search grounding đều bị từ chối.

Tài liệu Gemini: [Audio understanding](https://ai.google.dev/gemini-api/docs/generate-content/audio), [Grounding with Google Search](https://ai.google.dev/gemini-api/docs/google-search), [Speech generation](https://ai.google.dev/gemini-api/docs/speech-generation).
