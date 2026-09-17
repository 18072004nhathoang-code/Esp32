/**
 * @file audio_manager.cpp
 * @brief Triển khai hệ thống âm thanh I2S Duplex cho ES3C28P ESP32-S3 2.8" (XiaoZhi AI)
 * Tích hợp Microphone MEMS, Bộ khuếch đại Loa PA FM8002E/ES8311, Synthesizer & PSRAM Recorder
 */

#include "audio_manager.h"
#include "shared_i2c_bus.h"
#include <driver/i2s.h>
#include <esp_heap_caps.h>
#include <math.h>
#include "../storage/storage_manager.h"

// Quản lý trạng thái hệ thống âm thanh
static bool is_initialized = false;
static uint8_t master_volume = 80; // 0 - 100%
static bool pa_enabled = true;

// Bộ đệm ghi âm trong Octal PSRAM (8MB)
static int16_t *psram_record_buf = nullptr;
static uint32_t record_sample_capacity = AUDIO_MAX_SAMPLES;
static uint32_t recorded_samples_count = 0;
static bool recording_active = false;

// Trạng thái phát lại
static bool playback_active = false;
static uint32_t playback_sample_idx = 0;
static volatile AudioRecordingFileState recording_file_state = AUDIO_FILE_NONE;
static TaskHandle_t recording_export_task_handle = nullptr;
static constexpr const char *kRecordingPath = "/voice/last_recording.wav";

// Đo lường Microphone thời gian thực
static uint8_t current_mic_level = 0;     // 0 - 100%
static float current_mic_db = -60.0f;     // -60 to 0 dB
static int16_t waveform_history[128] = {0};
static size_t waveform_head = 0;

// FreeRTOS Task & Mutex
static TaskHandle_t audio_task_handle = nullptr;
static SemaphoreHandle_t audio_i2s_tx_mutex = nullptr; // Mutex độc quyền đường truyền TX i2s_write
// Protects recorder/playback state, the shared PSRAM buffer and waveform telemetry
// across the audio worker (core 0) and LVGL/application tasks (core 1).
static SemaphoreHandle_t audio_state_mutex = nullptr;

// Máy trạng thái phân quyền I2S phần cứng (Exclusive Ownership với RefCount Lease)
static volatile AudioOwner current_audio_owner = AUDIO_OWNER_NONE;
static volatile uint32_t audio_owner_refcount = 0;
static SemaphoreHandle_t audio_owner_mutex = nullptr;
static bool i2s_duplex_installed = false;

// Máy trạng thái đồng bộ hóa an toàn vòng đời Audio Task (Handshake/State Machine)
enum AudioTaskState
{
    AUDIO_TASK_ACTIVE = 0,
    AUDIO_TASK_PAUSE_REQUESTED,
    AUDIO_TASK_PAUSED,
    AUDIO_TASK_RESUME_REQUESTED
};
static volatile AudioTaskState audio_task_state = AUDIO_TASK_ACTIVE;
static SemaphoreHandle_t audio_task_ack_sem = nullptr;

static void put_le16(uint8_t *p, uint16_t value)
{
    p[0] = value & 0xFF;
    p[1] = (value >> 8) & 0xFF;
}

static void put_le32(uint8_t *p, uint32_t value)
{
    p[0] = value & 0xFF;
    p[1] = (value >> 8) & 0xFF;
    p[2] = (value >> 16) & 0xFF;
    p[3] = (value >> 24) & 0xFF;
}

static void recording_export_task(void *)
{
    bool ok = false;
    uint32_t export_sample_count = 0;
    if (audio_state_mutex &&
        xSemaphoreTake(audio_state_mutex, pdMS_TO_TICKS(100)) == pdTRUE)
    {
        export_sample_count = recorded_samples_count;
        xSemaphoreGive(audio_state_mutex);
    }
    if (storage_is_available() && storage_lock(1000))
    {
        fs::FS &fs = storage_get_fs();
        bool directory_ready = fs.exists("/voice") || fs.mkdir("/voice");
        bool old_file_removed = !fs.exists(kRecordingPath) || fs.remove(kRecordingPath);
        File file = (directory_ready && old_file_removed) ? fs.open(kRecordingPath, FILE_WRITE) : File();
        if (file)
        {
            const uint32_t data_bytes = export_sample_count * sizeof(int16_t);
            uint8_t header[44] = {};
            memcpy(header, "RIFF", 4); put_le32(header + 4, 36 + data_bytes);
            memcpy(header + 8, "WAVEfmt ", 8); put_le32(header + 16, 16);
            put_le16(header + 20, 1); put_le16(header + 22, 1);
            put_le32(header + 24, AUDIO_SAMPLE_RATE);
            put_le32(header + 28, AUDIO_SAMPLE_RATE * sizeof(int16_t));
            put_le16(header + 32, sizeof(int16_t)); put_le16(header + 34, 16);
            memcpy(header + 36, "data", 4); put_le32(header + 40, data_bytes);
            ok = file.write(header, sizeof(header)) == sizeof(header);
            const uint8_t *raw = reinterpret_cast<const uint8_t *>(psram_record_buf);
            size_t remaining = data_bytes;
            while (ok && remaining > 0)
            {
                size_t chunk = remaining > 4096 ? 4096 : remaining;
                ok = file.write(raw, chunk) == chunk;
                raw += chunk;
                remaining -= chunk;
                vTaskDelay(1);
            }
            file.close();
        }
        storage_unlock();
    }
    if (audio_state_mutex &&
        xSemaphoreTake(audio_state_mutex, pdMS_TO_TICKS(100)) == pdTRUE)
    {
        recording_file_state = ok ? AUDIO_FILE_SAVED : AUDIO_FILE_ERROR;
        recording_export_task_handle = nullptr;
        xSemaphoreGive(audio_state_mutex);
    }
    vTaskDelete(nullptr);
}

static void schedule_recording_export(void)
{
    if (!audio_state_mutex ||
        xSemaphoreTake(audio_state_mutex, pdMS_TO_TICKS(100)) != pdTRUE) return;
    if (recorded_samples_count == 0 || recording_export_task_handle)
    {
        xSemaphoreGive(audio_state_mutex);
        return;
    }
    if (!storage_is_available())
    {
        recording_file_state = AUDIO_FILE_ERROR;
        xSemaphoreGive(audio_state_mutex);
        return;
    }
    recording_file_state = AUDIO_FILE_SAVING;
    BaseType_t created = xTaskCreatePinnedToCore(recording_export_task, "VoiceWavSave", 4096,
                                                 nullptr, 1, &recording_export_task_handle, 0);
    if (created != pdPASS)
    {
        recording_export_task_handle = nullptr;
        recording_file_state = AUDIO_FILE_ERROR;
    }
    xSemaphoreGive(audio_state_mutex);
}

/* Cấu hình và cài đặt Driver I2S Duplex (16kHz 16-bit Duplex) cho Microphone & Tone/Voice */
bool audio_install_duplex_driver(void)
{
    if (i2s_duplex_installed) return true;

    i2s_config_t i2s_config = {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX | I2S_MODE_RX),
        .sample_rate = AUDIO_SAMPLE_RATE,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
        .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count = 6,
        .dma_buf_len = 128,
        .use_apll = true,
        .tx_desc_auto_clear = true,
        .fixed_mclk = 0
    };

    i2s_pin_config_t pin_config = {
        .mck_io_num = AUDIO_I2S_MCLK,
        .bck_io_num = AUDIO_I2S_BCLK,
        .ws_io_num = AUDIO_I2S_WS,
        .data_out_num = AUDIO_I2S_DOUT,
        .data_in_num = AUDIO_I2S_DIN
    };

    esp_err_t err = i2s_driver_install(I2S_NUM_0, &i2s_config, 0, NULL);
    if (err != ESP_OK)
    {
        Serial.printf("[AUDIO] ❌ LỖI cài đặt I2S Duplex Driver: 0x%X\n", err);
        return false;
    }

    err = i2s_set_pin(I2S_NUM_0, &pin_config);
    if (err != ESP_OK)
    {
        Serial.printf("[AUDIO] ❌ LỖI gán chân I2S: 0x%X\n", err);
        i2s_driver_uninstall(I2S_NUM_0);
        return false;
    }

    i2s_duplex_installed = true;
    Serial.println("[AUDIO] ✔ Đã cài đặt I2S Duplex Driver (16kHz TX+RX) sẵn sàng.");
    return true;
}

void audio_uninstall_duplex_driver(void)
{
    if (!i2s_duplex_installed) return;

    i2s_zero_dma_buffer(I2S_NUM_0);
    esp_err_t err = i2s_driver_uninstall(I2S_NUM_0);
    if (err == ESP_OK)
    {
        i2s_duplex_installed = false;
        Serial.println("[AUDIO] 🔌 Đã gỡ bỏ I2S Duplex Driver để nhường cổng I2S_NUM_0.");
    }
    else
    {
        Serial.printf("[AUDIO] Cảnh báo khi gỡ I2S Driver: 0x%X\n", err);
    }
}

bool audio_is_driver_installed(void)
{
    return i2s_duplex_installed;
}

bool audio_manager_pause_task_sync(uint32_t timeout_ms)
{
    if (audio_task_handle == nullptr) return true;
    if (audio_task_state == AUDIO_TASK_PAUSED) return true;

    if (audio_task_ack_sem == nullptr)
    {
        audio_task_ack_sem = xSemaphoreCreateBinary();
    }
    if (audio_task_ack_sem == nullptr)
    {
        Serial.println("[AUDIO] ❌ Chế độ suy giảm: không tạo được semaphore đồng bộ");
        return false;
    }
    xSemaphoreTake(audio_task_ack_sem, 0); // Dọn sạch token cũ nếu còn

    audio_task_state = AUDIO_TASK_PAUSE_REQUESTED;

    // Đợi Audio Task gửi ACK xác nhận đã ra khỏi mọi hàm I2S DMA
    if (xSemaphoreTake(audio_task_ack_sem, pdMS_TO_TICKS(timeout_ms)) == pdTRUE)
    {
        Serial.println("[AUDIO] 🛑 Audio Task đã dừng an toàn và gửi ACK.");
        return true;
    }
    Serial.println("[AUDIO] ⚠️ Timeout chờ ACK dừng Audio Task!");
    return (audio_task_state == AUDIO_TASK_PAUSED);
}

void audio_manager_resume_task(void)
{
    if (audio_task_state == AUDIO_TASK_PAUSED || audio_task_state == AUDIO_TASK_PAUSE_REQUESTED)
    {
        audio_task_state = AUDIO_TASK_RESUME_REQUESTED;
        Serial.println("[AUDIO] ▶ Đã gửi yêu cầu khôi phục hoạt động cho Audio Task.");
    }
}

bool audio_request_ownership(AudioOwner requester)
{
    if (audio_owner_mutex == nullptr)
    {
        audio_owner_mutex = xSemaphoreCreateMutex();
    }
    if (audio_owner_mutex == nullptr)
    {
        Serial.println("[AUDIO] ❌ Không tạo được mutex quản lý quyền I2S");
        return false;
    }
    if (xSemaphoreTake(audio_owner_mutex, pdMS_TO_TICKS(150)) != pdTRUE)
    {
        return false;
    }

    // Nếu chính requester này đang giữ lease: tăng refcount
    if (current_audio_owner == requester)
    {
        audio_owner_refcount++;
        xSemaphoreGive(audio_owner_mutex);
        return true;
    }

    // NGUYÊN TẮC BẮT BUỘC: Độc quyền thực sự (Exclusive Arbitration)
    // Không cho SYSTEM, RECORDER, AI_VOICE, MUSIC chiếm khi chủ sở hữu khác đang bận
    if (current_audio_owner != AUDIO_OWNER_NONE)
    {
        xSemaphoreGive(audio_owner_mutex);
        return false;
    }

    // 1. Phân hệ MUSIC (ESP32-audioI2S) yêu cầu độc quyền I2S_NUM_0
    if (requester == AUDIO_OWNER_MUSIC)
    {
        // Tạm dừng phát âm thanh hệ thống (nếu có)
        playback_active = false;

        // BƯỚC BẮT BUỘC: Đồng bộ dừng hoàn toàn Audio Task và chờ ACK trước khi gỡ driver
        if (!audio_manager_pause_task_sync(300))
        {
            Serial.println("[AUDIO] ❌ Lỗi: Không thể pause Audio Task kịp thời để nhường I2S cho MUSIC!");
            xSemaphoreGive(audio_owner_mutex);
            return false;
        }

        // Gỡ bỏ I2S driver của AudioManager khi chắc chắn không còn tác vụ nào gọi i2s_read/i2s_write
        audio_uninstall_duplex_driver();

        current_audio_owner = AUDIO_OWNER_MUSIC;
        audio_owner_refcount = 1;
        xSemaphoreGive(audio_owner_mutex);
        return true;
    }

    // 2. Đối với các requester khác (SYSTEM, RECORDER, AI_VOICE):
    // Đảm bảo I2S Duplex Driver của AudioManager đã sẵn sàng
    if (!i2s_duplex_installed)
    {
        if (!audio_install_duplex_driver())
        {
            xSemaphoreGive(audio_owner_mutex);
            return false;
        }
    }

    current_audio_owner = requester;
    audio_owner_refcount = 1;
    xSemaphoreGive(audio_owner_mutex);
    return true;
}

void audio_release_ownership(AudioOwner requester)
{
    if (audio_owner_mutex && xSemaphoreTake(audio_owner_mutex, pdMS_TO_TICKS(150)) == pdTRUE)
    {
        if (current_audio_owner == requester)
        {
            if (audio_owner_refcount > 1)
            {
                audio_owner_refcount--;
            }
            else
            {
                audio_owner_refcount = 0;
                current_audio_owner = AUDIO_OWNER_NONE;

                // Nếu MUSIC vừa nhả quyền sở hữu: Cài đặt lại I2S Duplex Driver rồi đánh thức Audio Task
                if (requester == AUDIO_OWNER_MUSIC)
                {
                    if (audio_install_duplex_driver())
                    {
                        audio_manager_resume_task();
                    }
                    else
                    {
                        Serial.println("[AUDIO] ❌ Chế độ suy giảm: MUSIC đã nhả lease nhưng I2S duplex chưa khôi phục được");
                    }
                }
            }
        }
        xSemaphoreGive(audio_owner_mutex);
    }
}

AudioOwner audio_get_current_owner(void)
{
    return current_audio_owner;
}

/* =========================================================================
 * CẤU HÌNH VÀ GHI DỮ LIỆU I2C CODEC ES8311 (NẾU CÓ)
 * ========================================================================= */
static bool es8311_write_reg(uint8_t reg, uint8_t val)
{
    return shared_i2c_write_reg(AUDIO_ES8311_ADDR, reg, val);
}

static bool es8311_init_codec(void)
{
    if (!shared_i2c_codec_is_detected())
    {
        Serial.println("[AUDIO] Không phát hiện chip ES8311 qua I2C. Chuyển sang Direct I2S Mode.");
        return false;
    }

    Serial.println("[AUDIO] Đã nhận diện chip Codec ES8311! Đang khởi tạo thanh ghi...");
    // Khởi tạo cơ bản thanh ghi ES8311 qua Shared I2C Bus an toàn
    es8311_write_reg(0x00, 0x1F); // CSM on, reset
    es8311_write_reg(0x01, 0x30); // Clock manager
    es8311_write_reg(0x02, 0x00); // Clock inverted/pol
    es8311_write_reg(0x03, 0x10); // ADC / DAC SCLK divider
    es8311_write_reg(0x0D, 0x01); // Power up analog
    es8311_write_reg(0x0E, 0x02); // Power up analog
    es8311_write_reg(0x12, 0x00); // Enable ADC
    es8311_write_reg(0x13, 0x10); // ADC PGA gain (+18dB)
    es8311_write_reg(0x14, 0x1A); // ADC Gain Boost (+30dB cho MEMS mic)
    es8311_write_reg(0x31, 0x00); // Power up DAC
    es8311_write_reg(0x32, 0xBF); // DAC Digital Volume (mặc định ~75%)
    return true;
}

/* =========================================================================
 * BẬT/TẮT POWER AMPLIFIER (FM8002E / NS4168)
 * ========================================================================= */
void audio_set_pa_enabled(bool enabled)
{
    pa_enabled = enabled;
    // PA pin là Active LOW (Mức 0 = Mở loa, Mức 1 = Tắt loa)
    digitalWrite(AUDIO_PA_PIN, enabled ? LOW : HIGH);
}

bool audio_is_pa_enabled(void)
{
    return pa_enabled;
}

void audio_set_volume(uint8_t volume_percent)
{
    if (volume_percent > 100) volume_percent = 100;
    master_volume = volume_percent;

    // Cập nhật mức volume vào thanh ghi ES8311 nếu có
    uint8_t reg_vol = (uint8_t)((volume_percent * 255) / 100);
    es8311_write_reg(0x32, reg_vol);
}

uint8_t audio_get_volume(void)
{
    return master_volume;
}

/* =========================================================================
 * FREERTOS AUDIO TASK CHẠY TRÊN CORE 0 (I2S DMA SAMPLING & RECORD/PLAYBACK)
 * ========================================================================= */
static void audio_background_task(void *pvParameters)
{
    const size_t DMA_READ_LEN = 128; // Số mẫu đọc mỗi lần từ DMA
    int16_t rx_buf[DMA_READ_LEN * 2]; // Stereo / 2-channel buffer
    size_t bytes_read = 0;

    while (1)
    {
        // 0a. Máy trạng thái Handshake dừng/khôi phục Audio Task an toàn
        if (audio_task_state == AUDIO_TASK_PAUSE_REQUESTED)
        {
            audio_task_state = AUDIO_TASK_PAUSED;
            if (audio_task_ack_sem)
            {
                xSemaphoreGive(audio_task_ack_sem);
            }
        }

        if (audio_task_state == AUDIO_TASK_PAUSED)
        {
            vTaskDelay(pdMS_TO_TICKS(15));
            continue;
        }

        if (audio_task_state == AUDIO_TASK_RESUME_REQUESTED)
        {
            audio_task_state = AUDIO_TASK_ACTIVE;
        }

        // 0b. Nếu I2S đang được Music Player sử dụng hoặc driver chưa cài đặt, nhường bus hoàn toàn
        if (current_audio_owner == AUDIO_OWNER_MUSIC || !i2s_duplex_installed)
        {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        // 1. Đọc luồng âm thanh đầu vào từ Microphone MEMS qua I2S RX
        esp_err_t err = i2s_read(I2S_NUM_0, rx_buf, sizeof(rx_buf), &bytes_read, pdMS_TO_TICKS(15));
        if (err == ESP_OK && bytes_read > 0)
        {
            size_t sample_count = bytes_read / sizeof(int16_t);
            int64_t sum_squares = 0;
            int16_t peak = 0;
            bool recording_complete = false;

            const bool state_locked = audio_state_mutex &&
                                      xSemaphoreTake(audio_state_mutex, pdMS_TO_TICKS(20)) == pdTRUE;

            for (size_t i = 0; i < sample_count; i += 2) // Lấy kênh Mic (thường là Left hoặc Right)
            {
                int16_t s = rx_buf[i];
                int16_t abs_s = (s < 0) ? -s : s;
                if (abs_s > peak) peak = abs_s;
                sum_squares += ((int32_t)s * (int32_t)s);

                // Lưu vào lịch sử waveform
                if (state_locked)
                {
                    waveform_history[waveform_head] = s;
                    waveform_head = (waveform_head + 1) % 128;
                }

                // Nếu đang ghi âm: lưu trực tiếp vào bộ nhớ 8MB PSRAM
                if (state_locked && recording_active && psram_record_buf &&
                    recorded_samples_count < record_sample_capacity)
                {
                    psram_record_buf[recorded_samples_count++] = s;
                }
                else if (state_locked && recording_active &&
                         recorded_samples_count >= record_sample_capacity)
                {
                    recording_active = false;
                    recording_complete = true;
                }
            }

            // Tính toán RMS Level (0 - 100%) và Decibel
            size_t half_samples = sample_count / 2;
            if (half_samples > 0)
            {
                float rms = sqrtf((float)sum_squares / (float)half_samples);
                // Giới hạn tỉ lệ 0 - 100% dựa trên biên độ 16-bit
                float level = (rms / 4000.0f) * 100.0f;
                if (level > 100.0f) level = 100.0f;
                current_mic_level = (uint8_t)level;

                if (rms > 1.0f)
                {
                    current_mic_db = 20.0f * log10f(rms / 32768.0f);
                    if (current_mic_db < -60.0f) current_mic_db = -60.0f;
                }
                else
                {
                    current_mic_db = -60.0f;
                }
            }

            if (state_locked) xSemaphoreGive(audio_state_mutex);
            if (recording_complete)
            {
                schedule_recording_export();
                audio_release_ownership(AUDIO_OWNER_RECORDER);
            }
        }

        // 2. Nếu đang phát lại đoạn ghi âm qua I2S TX
        bool playback_complete = false;
        if (audio_state_mutex &&
            xSemaphoreTake(audio_state_mutex, pdMS_TO_TICKS(20)) == pdTRUE)
        {
            if (playback_active && psram_record_buf && recorded_samples_count > 0)
            {
                const size_t PLAY_CHUNK = 64;
                int16_t tx_buf[PLAY_CHUNK * 2];
                size_t to_play = PLAY_CHUNK;
                if (playback_sample_idx + to_play > recorded_samples_count)
                {
                    to_play = recorded_samples_count - playback_sample_idx;
                }

                if (to_play > 0)
                {
                    for (size_t i = 0; i < to_play; i++)
                    {
                        int16_t raw_sample = psram_record_buf[playback_sample_idx + i];
                        // Nhân hệ số âm lượng Master Volume
                        int32_t scaled = ((int32_t)raw_sample * master_volume) / 100;
                        if (scaled > 32767) scaled = 32767;
                        if (scaled < -32768) scaled = -32768;

                        tx_buf[i * 2]     = (int16_t)scaled; // Left
                        tx_buf[i * 2 + 1] = (int16_t)scaled; // Right
                    }

                    if (audio_i2s_tx_mutex &&
                        xSemaphoreTake(audio_i2s_tx_mutex, pdMS_TO_TICKS(50)) == pdTRUE)
                    {
                        size_t bytes_written = 0;
                        const esp_err_t write_err = i2s_write(
                            I2S_NUM_0, tx_buf, to_play * 2 * sizeof(int16_t),
                            &bytes_written, pdMS_TO_TICKS(20));
                        xSemaphoreGive(audio_i2s_tx_mutex);
                        if (write_err == ESP_OK && bytes_written == to_play * 2 * sizeof(int16_t))
                        {
                            playback_sample_idx += to_play;
                        }
                    }
                }

                if (playback_sample_idx >= recorded_samples_count)
                {
                    playback_active = false;
                    playback_sample_idx = 0;
                    playback_complete = true;
                }
            }
            xSemaphoreGive(audio_state_mutex);
        }
        if (playback_complete)
        {
            audio_release_ownership(AUDIO_OWNER_SYSTEM);
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

/* =========================================================================
 * KHỞI TẠO HỆ THỐNG AUDIO DRIVER
 * ========================================================================= */
bool audio_manager_init(void)
{
    if (is_initialized) return true;

    Serial.println("\n[AUDIO] Khởi tạo hệ thống Âm thanh I2S Duplex (Mic & Speaker)...");

    // 1. Cấu hình chân Power Amplifier (FM8002E / NS4168)
    pinMode(AUDIO_PA_PIN, OUTPUT);
    audio_set_pa_enabled(true);

    // 2. Cài đặt Driver I2S Duplex (16kHz 16-bit Master TX + RX)
    if (!audio_install_duplex_driver())
    {
        Serial.println("[AUDIO] ❌ Lỗi khởi tạo I2S Duplex Driver!");
        return false;
    }

    // 3. Khởi tạo Codec ES8311 qua I2C nếu có trên mạch
    bool has_codec = es8311_init_codec();
    if (has_codec)
    {
        Serial.println("[AUDIO] ✔ ES8311 Codec được cấu hình thành công");
    }
    audio_set_volume(80);

    // 4. Cấp phát bộ đệm ghi âm 320KB trong 8MB Octal PSRAM
    if (psramFound())
    {
        psram_record_buf = (int16_t *)heap_caps_malloc(AUDIO_MAX_SAMPLES * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (psram_record_buf)
        {
            Serial.printf("[AUDIO] Đã cấp phát %d KB PSRAM cho Voice Recorder (10 giây 16kHz)\n",
                          (AUDIO_MAX_SAMPLES * sizeof(int16_t)) / 1024);
        }
        else
        {
            Serial.println("[AUDIO] Cảnh báo: Không thể cấp phát bộ đệm PSRAM.");
        }
    }

    // 5. Khởi tạo Semaphore Handshake & FreeRTOS Task chạy trên Core 0 (Priority 3: Audio Realtime)
    if (audio_task_ack_sem == nullptr)
    {
        audio_task_ack_sem = xSemaphoreCreateBinary();
    }
    if (audio_task_ack_sem == nullptr)
    {
        Serial.println("[AUDIO] ❌ Chế độ suy giảm: không tạo được semaphore Audio Task");
        audio_uninstall_duplex_driver();
        return false;
    }

    if (audio_owner_mutex == nullptr) audio_owner_mutex = xSemaphoreCreateMutex();
    if (audio_i2s_tx_mutex == nullptr) audio_i2s_tx_mutex = xSemaphoreCreateMutex();
    if (audio_state_mutex == nullptr) audio_state_mutex = xSemaphoreCreateMutex();
    if (!audio_owner_mutex || !audio_i2s_tx_mutex || !audio_state_mutex)
    {
        Serial.println("[AUDIO] ❌ Chế độ suy giảm: không tạo được mutex audio");
        audio_uninstall_duplex_driver();
        return false;
    }

    BaseType_t task_ret = xTaskCreatePinnedToCore(
        audio_background_task,
        "Audio_Task",
        4096,
        NULL,
        3, // Priority 3: Audio Realtime
        &audio_task_handle,
        0  // Core 0 (để Core 1 chuyên cho LVGL Display)
    );

    if (task_ret != pdPASS)
    {
        Serial.println("[AUDIO] ❌ Không thể tạo Audio_Task trên Core 0!");
        audio_uninstall_duplex_driver();
        return false;
    }

    is_initialized = true;
    Serial.println("[AUDIO] Hệ thống âm thanh đã sẵn sàng trên Core 0!");
    return true;
}

/* =========================================================================
 * BỘ TỔNG HỢP ÂM THANH (TONE & SOUNDBOARD SYNTHESIZER)
 * ========================================================================= */
void audio_play_tone(uint32_t freq_hz, uint32_t duration_ms)
{
    if (!is_initialized || freq_hz == 0 || duration_ms == 0) return;
    if (!audio_request_ownership(AUDIO_OWNER_SYSTEM)) return;

    if (audio_i2s_tx_mutex == nullptr)
    {
        audio_i2s_tx_mutex = xSemaphoreCreateMutex();
    }
    if (!audio_i2s_tx_mutex || xSemaphoreTake(audio_i2s_tx_mutex, pdMS_TO_TICKS(200)) != pdTRUE)
    {
        audio_release_ownership(AUDIO_OWNER_SYSTEM);
        return;
    }

    size_t total_samples = (AUDIO_SAMPLE_RATE * duration_ms) / 1000;
    const size_t CHUNK_SIZE = 128;
    int16_t buffer[CHUNK_SIZE * 2];

    float phase = 0.0f;
    float phase_step = (2.0f * (float)M_PI * (float)freq_hz) / (float)AUDIO_SAMPLE_RATE;
    float max_amp = (32767.0f * (float)master_volume) / 100.0f;

    size_t samples_generated = 0;
    while (samples_generated < total_samples)
    {
        size_t count = CHUNK_SIZE;
        if (samples_generated + count > total_samples)
        {
            count = total_samples - samples_generated;
        }

        for (size_t i = 0; i < count; i++)
        {
            // Làm mượt đầu và đuôi sóng (Envelope Attack/Decay) để không bị "bụp" loa
            float env = 1.0f;
            size_t cur = samples_generated + i;
            if (cur < 100) env = (float)cur / 100.0f;
            else if (total_samples - cur < 100) env = (float)(total_samples - cur) / 100.0f;

            int16_t sample_val = (int16_t)(sinf(phase) * max_amp * env);
            buffer[i * 2]     = sample_val; // Left
            buffer[i * 2 + 1] = sample_val; // Right

            phase += phase_step;
            if (phase >= 2.0f * (float)M_PI) phase -= 2.0f * (float)M_PI;
        }

        size_t bytes_written = 0;
        i2s_write(I2S_NUM_0, buffer, count * 2 * sizeof(int16_t), &bytes_written, portMAX_DELAY);
        samples_generated += count;
    }

    xSemaphoreGive(audio_i2s_tx_mutex);
    audio_release_ownership(AUDIO_OWNER_SYSTEM);
}

void audio_play_sound_effect(SoundEffect fx)
{
    switch (fx)
    {
        case FX_CLICK:
            audio_play_tone(1600, 20);
            break;
        case FX_BEEP:
            audio_play_tone(2400, 100);
            break;
        case FX_CHIME:
            audio_play_tone(523, 80);  // C5
            audio_play_tone(659, 80);  // E5
            audio_play_tone(784, 120); // G5
            break;
        case FX_MELODY:
            audio_play_tone(523, 70);  // C5
            audio_play_tone(659, 70);  // E5
            audio_play_tone(784, 70);  // G5
            audio_play_tone(1046, 140); // C6
            break;
        case FX_XIAOZHI_WAKE:
            audio_play_tone(880, 80);  // A5
            vTaskDelay(pdMS_TO_TICKS(20));
            audio_play_tone(1760, 120); // A6
            break;
        default:
            break;
    }
}

/* =========================================================================
 * BỘ GHI ÂM VÀ PHÁT LẠI (VOICE MEMO / PSRAM BUFFER)
 * ========================================================================= */
bool audio_start_recording(uint32_t max_duration_sec)
{
    if (!psram_record_buf || !audio_state_mutex || max_duration_sec == 0) return false;

    if (xSemaphoreTake(audio_state_mutex, pdMS_TO_TICKS(100)) != pdTRUE) return false;
    const bool export_active = recording_export_task_handle != nullptr;
    const bool already_recording = recording_active;
    xSemaphoreGive(audio_state_mutex);
    if (export_active) return false;
    if (already_recording) return true;

    audio_stop_playback();
    if (!audio_request_ownership(AUDIO_OWNER_RECORDER)) return false;

    if (xSemaphoreTake(audio_state_mutex, pdMS_TO_TICKS(100)) != pdTRUE)
    {
        audio_release_ownership(AUDIO_OWNER_RECORDER);
        return false;
    }
    record_sample_capacity = AUDIO_SAMPLE_RATE * max_duration_sec;
    if (record_sample_capacity > AUDIO_MAX_SAMPLES)
    {
        record_sample_capacity = AUDIO_MAX_SAMPLES;
    }
    recorded_samples_count = 0;
    recording_file_state = AUDIO_FILE_NONE;
    recording_active = true;
    xSemaphoreGive(audio_state_mutex);
    Serial.printf("[AUDIO] Bắt đầu ghi âm Mic vào PSRAM (Tối đa %u giây)...\n", max_duration_sec);
    return true;
}

void audio_stop_recording(void)
{
    if (!audio_state_mutex ||
        xSemaphoreTake(audio_state_mutex, pdMS_TO_TICKS(100)) != pdTRUE) return;
    const bool was_recording = recording_active;
    recording_active = false;
    const uint32_t sample_count = recorded_samples_count;
    xSemaphoreGive(audio_state_mutex);

    if (was_recording)
    {
        Serial.printf("[AUDIO] Đã dừng ghi âm. Thu được %u mẫu (%.2f giây)\n",
                      sample_count, (float)sample_count / AUDIO_SAMPLE_RATE);
        schedule_recording_export();
        audio_release_ownership(AUDIO_OWNER_RECORDER);
    }
}

void audio_cancel_recording(void)
{
    if (!audio_state_mutex ||
        xSemaphoreTake(audio_state_mutex, pdMS_TO_TICKS(100)) != pdTRUE) return;
    const bool was_recording = recording_active;
    recording_active = false;
    recorded_samples_count = 0;
    recording_file_state = AUDIO_FILE_NONE;
    xSemaphoreGive(audio_state_mutex);
    if (was_recording) audio_release_ownership(AUDIO_OWNER_RECORDER);
}

bool audio_is_recording(void)
{
    if (!audio_state_mutex ||
        xSemaphoreTake(audio_state_mutex, pdMS_TO_TICKS(20)) != pdTRUE) return false;
    const bool active = recording_active;
    xSemaphoreGive(audio_state_mutex);
    return active;
}

bool audio_start_playback(void)
{
    if (!psram_record_buf || !audio_state_mutex) return false;
    audio_stop_recording();

    if (xSemaphoreTake(audio_state_mutex, pdMS_TO_TICKS(100)) != pdTRUE) return false;
    const bool already_playing = playback_active;
    const bool has_samples = recorded_samples_count > 0;
    xSemaphoreGive(audio_state_mutex);
    if (already_playing) return true;
    if (!has_samples) return false;

    if (!audio_request_ownership(AUDIO_OWNER_SYSTEM)) return false;

    if (xSemaphoreTake(audio_state_mutex, pdMS_TO_TICKS(100)) != pdTRUE)
    {
        audio_release_ownership(AUDIO_OWNER_SYSTEM);
        return false;
    }
    playback_sample_idx = 0;
    playback_active = true;
    const uint32_t sample_count = recorded_samples_count;
    xSemaphoreGive(audio_state_mutex);
    Serial.printf("[AUDIO] Bắt đầu phát lại đoạn ghi âm (%u mẫu)...\n", sample_count);
    return true;
}

void audio_stop_playback(void)
{
    if (!audio_state_mutex ||
        xSemaphoreTake(audio_state_mutex, pdMS_TO_TICKS(100)) != pdTRUE) return;
    const bool was_playing = playback_active;
    playback_active = false;
    playback_sample_idx = 0;
    xSemaphoreGive(audio_state_mutex);
    if (was_playing) audio_release_ownership(AUDIO_OWNER_SYSTEM);
}

bool audio_is_playing(void)
{
    if (!audio_state_mutex ||
        xSemaphoreTake(audio_state_mutex, pdMS_TO_TICKS(20)) != pdTRUE) return false;
    const bool active = playback_active;
    xSemaphoreGive(audio_state_mutex);
    return active;
}

uint32_t audio_get_recorded_duration_ms(void)
{
    if (!audio_state_mutex ||
        xSemaphoreTake(audio_state_mutex, pdMS_TO_TICKS(20)) != pdTRUE) return 0;
    const uint32_t duration = (recorded_samples_count * 1000) / AUDIO_SAMPLE_RATE;
    xSemaphoreGive(audio_state_mutex);
    return duration;
}

uint32_t audio_get_playback_progress_ms(void)
{
    if (!audio_state_mutex ||
        xSemaphoreTake(audio_state_mutex, pdMS_TO_TICKS(20)) != pdTRUE) return 0;
    const uint32_t progress = (playback_sample_idx * 1000) / AUDIO_SAMPLE_RATE;
    xSemaphoreGive(audio_state_mutex);
    return progress;
}

size_t audio_get_recorded_sample_count(void)
{
    if (!audio_state_mutex ||
        xSemaphoreTake(audio_state_mutex, pdMS_TO_TICKS(20)) != pdTRUE) return 0;
    const size_t count = recording_active ? 0 : recorded_samples_count;
    xSemaphoreGive(audio_state_mutex);
    return count;
}

size_t audio_copy_recorded_samples(size_t offset, int16_t *dest, size_t max_samples)
{
    if (!dest || max_samples == 0 || !audio_state_mutex ||
        xSemaphoreTake(audio_state_mutex, pdMS_TO_TICKS(100)) != pdTRUE) return 0;
    if (recording_active || !psram_record_buf || offset >= recorded_samples_count)
    {
        xSemaphoreGive(audio_state_mutex);
        return 0;
    }
    size_t count = recorded_samples_count - offset;
    if (count > max_samples) count = max_samples;
    memcpy(dest, psram_record_buf + offset, count * sizeof(int16_t));
    xSemaphoreGive(audio_state_mutex);
    return count;
}

AudioRecordingFileState audio_get_recording_file_state(void)
{
    if (!audio_state_mutex ||
        xSemaphoreTake(audio_state_mutex, pdMS_TO_TICKS(20)) != pdTRUE) return AUDIO_FILE_ERROR;
    const AudioRecordingFileState state = recording_file_state;
    xSemaphoreGive(audio_state_mutex);
    return state;
}

const char *audio_get_recording_file_path(void)
{
    return audio_get_recording_file_state() == AUDIO_FILE_SAVED ? kRecordingPath : "";
}

bool audio_write_pcm16_mono(const int16_t *samples, size_t count, uint32_t timeout_ms)
{
    if (!samples || count == 0 || !i2s_duplex_installed || current_audio_owner == AUDIO_OWNER_NONE)
        return false;
    if (!audio_i2s_tx_mutex || xSemaphoreTake(audio_i2s_tx_mutex, pdMS_TO_TICKS(timeout_ms)) != pdTRUE)
        return false;

    bool ok = true;
    int16_t stereo[256];
    size_t offset = 0;
    while (offset < count)
    {
        size_t chunk = count - offset;
        if (chunk > 128) chunk = 128;
        for (size_t i = 0; i < chunk; ++i)
        {
            int32_t scaled = ((int32_t)samples[offset + i] * master_volume) / 100;
            stereo[i * 2] = (int16_t)scaled;
            stereo[i * 2 + 1] = (int16_t)scaled;
        }
        size_t written = 0;
        esp_err_t err = i2s_write(I2S_NUM_0, stereo, chunk * 2 * sizeof(int16_t),
                                  &written, pdMS_TO_TICKS(timeout_ms));
        if (err != ESP_OK || written != chunk * 2 * sizeof(int16_t))
        {
            ok = false;
            break;
        }
        offset += chunk;
    }
    xSemaphoreGive(audio_i2s_tx_mutex);
    return ok;
}

/* =========================================================================
 * DỮ LIỆU TELEMETRY MICROPHONE THỜI GIAN THỰC
 * ========================================================================= */
uint8_t audio_get_mic_level(void)
{
    if (!audio_state_mutex ||
        xSemaphoreTake(audio_state_mutex, pdMS_TO_TICKS(20)) != pdTRUE) return 0;
    const uint8_t level = current_mic_level;
    xSemaphoreGive(audio_state_mutex);
    return level;
}

float audio_get_mic_db(void)
{
    if (!audio_state_mutex ||
        xSemaphoreTake(audio_state_mutex, pdMS_TO_TICKS(20)) != pdTRUE) return -60.0f;
    const float db = current_mic_db;
    xSemaphoreGive(audio_state_mutex);
    return db;
}

void audio_get_waveform_samples(int16_t *dest, size_t count)
{
    if (!dest || count == 0) return;
    if (count > 128) count = 128;
    if (!audio_state_mutex ||
        xSemaphoreTake(audio_state_mutex, pdMS_TO_TICKS(20)) != pdTRUE)
    {
        memset(dest, 0, count * sizeof(int16_t));
        return;
    }

    size_t start = (waveform_head + 128 - count) % 128;
    for (size_t i = 0; i < count; i++)
    {
        dest[i] = waveform_history[(start + i) % 128];
    }
    xSemaphoreGive(audio_state_mutex);
}
