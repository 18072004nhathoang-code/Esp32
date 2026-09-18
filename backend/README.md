# ESP32 Mini OS AI gateway

Gateway cho app **AI Voice** hiện có, tách ba provider độc lập:

1. Gemini STT chỉ chép WAV PCM16 mono 16 kHz thành `transcript`.
2. DeepSeek V4.1 Flash (`deepseek-flash`) tạo câu trả lời tiếng Việt và action nhạc.
3. Gemini TTS đổi `reply` thành WAV PCM16 mono 16 kHz.

ESP32 chỉ gọi gateway qua `POST /v1/query` và `POST /v1/tts`; firmware không kết nối trực tiếp `api.deepseek.com`. API key DeepSeek/Gemini chỉ nằm trong `backend/.env`. DeepSeek Chat Completions không nhận WAV hoặc base64 audio, và `reasoning_content` không được đưa vào phản hồi/TTS.

## Chạy

Yêu cầu Node.js 20 trở lên và reverse proxy HTTPS có chứng chỉ hợp lệ.

```bash
cd backend
cp .env.example .env
npm ci
npm test
npm start
```

Cấu hình tối thiểu:

```dotenv
AI_LLM_PROVIDER=deepseek
DEEPSEEK_API_KEY=...
DEEPSEEK_BASE_URL=https://api.deepseek.com
DEEPSEEK_MODEL=deepseek-flash
DEEPSEEK_THINKING=false

AI_STT_PROVIDER=gemini
AI_TTS_PROVIDER=gemini
GEMINI_API_KEY=...

ESP_DEVICE_TOKEN=mot-token-ngau-nhien-toi-thieu-32-ky-tu
TTS_PREWARM_ENABLED=false
```

Không đổi `DEEPSEEK_MODEL` sang tên hiển thị sản phẩm. `deepseek-flash` là model ID API của DeepSeek V4.1 Flash. Thinking mặc định tắt để giảm độ trễ; chỉ bật rõ bằng `DEEPSEEK_THINKING=true`. Gateway không fallback âm thầm sang provider khác.

Gemini key chỉ được yêu cầu khi STT hoặc TTS chọn Gemini. Bản hiện tại triển khai adapter STT/TTS Gemini; provider khác bị từ chối rõ lúc khởi động thay vì giả thành công. DeepSeek API có tính phí token; Gemini STT và TTS có quota/chi phí riêng. Gateway không tự bật billing hoặc nạp tiền.

Backend mặc định bind `127.0.0.1:8787`. Public Internet chỉ đi qua reverse proxy HTTPS, ví dụ Caddy:

```caddyfile
assistant.example.com {
    reverse_proxy 127.0.0.1:8787
    request_body {
        max_size 1MB
    }
}
```

Sao chép CA PEM của chuỗi chứng chỉ gateway vào `AI_VOICE_CA_CERT`, đặt hai URL `/v1/query`, `/v1/tts` và cùng `ESP_DEVICE_TOKEN` trong `include/secrets.h`. Firmware fail-closed nếu URL HTTPS thiếu CA hoặc token. Không dùng `setInsecure()`.

## Giao thức

- `POST /v1/query`, `Content-Type: audio/wav`, bearer device token: WAV PCM16 mono 16 kHz, tối đa 400.000 byte mặc định.
- Pipeline thực hiện STT → DeepSeek và trả `{"transcript":"...","reply":"...","actions":[],"sources":[]}`. `transcript` luôn lấy trực tiếp từ STT.
- `POST /v1/tts`, JSON đúng một trường `text`: Gemini TTS trả RIFF/WAV PCM16 mono 16 kHz.
- Chuỗi firmware tối đa 511 byte UTF-8, tối đa 2 action. DeepSeek không được tạo URL/source.
- Action duy nhất: `music.play`, `music.pause`, `music.resume`, `music.stop`, `music.volume` (0–100). `source_id` phải tồn tại trong cấu hình.
- Nếu câu hỏi cần dữ liệu mới, gateway trả lời rõ chưa có dịch vụ tìm kiếm; không chuyển `google_search` của Gemini sang DeepSeek, không bịa nguồn.
- Lỗi có dạng `{"error":{"code":"DEEPSEEK_AUTH","message":"...","provider_status":401}}`. 401/402 không retry; 429/5xx retry hữu hạn với backoff trong deadline tổng.
- Client đóng kết nối sẽ abort provider đang chạy. Firmware cũng đóng socket active khi người dùng Cancel nên response cũ không được phát hoặc thực thi.
- `MAX_CONCURRENT_REQUESTS` giới hạn pipeline đồng thời. Response provider được đọc theo byte limit, không chỉ dựa vào `Content-Length`.
- TTS prewarm mặc định tắt để tránh gọi trùng và phát sinh chi phí ngoài ý muốn.

## Nguồn nhạc

Khai báo cùng ID ở gateway và firmware. Model chỉ chọn ID đã cấu hình; firmware tự ánh xạ ID sang URL HTTPS cục bộ.

`backend/.env`:

```dotenv
MUSIC_SOURCES_JSON=[{"id":"radio1","label":"Radio 1","url":"https://radio.example/live.mp3"}]
```

`include/secrets.h`:

```c
#define AI_MUSIC_STREAM_SOURCES_JSON "[{\"id\":\"radio1\",\"url\":\"https://radio.example/live.mp3\"}]"
#define AI_MUSIC_STREAM_CA_CERT "-----BEGIN CERTIFICATE-----\n...\n-----END CERTIFICATE-----\n"
```

Thiết bị chỉ báo action thành công sau ACK thật từ Music Player. Không có source cấu hình thì `music.play` dùng bài hiện tại/đầu tiên trên SD; lỗi SD/decoder/I2S được trả đúng trạng thái.

Tài liệu provider: [DeepSeek API](https://api-docs.deepseek.com/), [DeepSeek Chat Completions](https://api-docs.deepseek.com/api/create-chat-completion/), [DeepSeek error codes](https://api-docs.deepseek.com/quick_start/error_codes/), [Gemini audio understanding](https://ai.google.dev/gemini-api/docs/audio), [Gemini speech generation](https://ai.google.dev/gemini-api/docs/speech-generation).
