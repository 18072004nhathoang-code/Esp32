/**
 * @file audio_manager.cpp
 * @brief Triển khai hệ thống âm thanh I2S Duplex cho DIYMORE ESP32-S3 3.5" (XiaoZhi AI)
 * Tích hợp Microphone MEMS, Bộ khuếch đại Loa PA FM8002E/ES8311, Synthesizer & PSRAM Recorder
 */

#include "audio_manager.h"
#include <driver/i2s.h>
#include <Wire.h>
#include <esp_heap_caps.h>
#include <math.h>

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

// Đo lường Microphone thời gian thực
static uint8_t current_mic_level = 0;     // 0 - 100%
static float current_mic_db = -60.0f;     // -60 to 0 dB
static int16_t waveform_history[128] = {0};
static size_t waveform_head = 0;

// FreeRTOS Task & Mutex
static TaskHandle_t audio_task_handle = nullptr;
static SemaphoreHandle_t audio_mutex = nullptr;

/* =========================================================================
 * CẤU HÌNH VÀ GHI DỮ LIỆU I2C CODEC ES8311 (NẾU CÓ)
 * ========================================================================= */
static bool es8311_write_reg(uint8_t reg, uint8_t val)
{
    Wire.beginTransmission(AUDIO_ES8311_ADDR);
    Wire.write(reg);
    Wire.write(val);
    return (Wire.endTransmission() == 0);
}

static bool es8311_init_codec(void)
{
    Wire.begin(AUDIO_I2C_SDA, AUDIO_I2C_SCL, 100000);
    Wire.beginTransmission(AUDIO_ES8311_ADDR);
    if (Wire.endTransmission() != 0)
    {
        Serial.println("[AUDIO] Không phát hiện chip ES8311 qua I2C. Chuyển sang Direct I2S Mode.");
        return false;
    }

    Serial.println("[AUDIO] Đã nhận diện chip Codec ES8311! Đang khởi tạo thanh ghi...");
    // Khởi tạo cơ bản thanh ghi ES8311
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
        // 1. Đọc luồng âm thanh đầu vào từ Microphone MEMS qua I2S RX
        esp_err_t err = i2s_read(I2S_NUM_0, rx_buf, sizeof(rx_buf), &bytes_read, pdMS_TO_TICKS(15));
        if (err == ESP_OK && bytes_read > 0)
        {
            size_t sample_count = bytes_read / sizeof(int16_t);
            int64_t sum_squares = 0;
            int16_t peak = 0;

            for (size_t i = 0; i < sample_count; i += 2) // Lấy kênh Mic (thường là Left hoặc Right)
            {
                int16_t s = rx_buf[i];
                int16_t abs_s = (s < 0) ? -s : s;
                if (abs_s > peak) peak = abs_s;
                sum_squares += ((int32_t)s * (int32_t)s);

                // Lưu vào lịch sử waveform
                waveform_history[waveform_head] = s;
                waveform_head = (waveform_head + 1) % 128;

                // Nếu đang ghi âm: lưu trực tiếp vào bộ nhớ 8MB PSRAM
                if (recording_active && psram_record_buf && recorded_samples_count < record_sample_capacity)
                {
                    psram_record_buf[recorded_samples_count++] = s;
                }
                else if (recording_active && recorded_samples_count >= record_sample_capacity)
                {
                    recording_active = false;
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
        }

        // 2. Nếu đang phát lại đoạn ghi âm qua I2S TX
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

                size_t bytes_written = 0;
                i2s_write(I2S_NUM_0, tx_buf, to_play * 2 * sizeof(int16_t), &bytes_written, pdMS_TO_TICKS(20));
                playback_sample_idx += to_play;
            }
            else
            {
                // Đã phát hết đoạn ghi âm
                playback_active = false;
                playback_sample_idx = 0;
            }
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

    // 2. Cấu hình I2S ESP-IDF Driver (Full Duplex Master TX + RX)
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
        Serial.printf("[AUDIO] LỖI cài đặt I2S Driver: 0x%X\n", err);
        return false;
    }

    err = i2s_set_pin(I2S_NUM_0, &pin_config);
    if (err != ESP_OK)
    {
        Serial.printf("[AUDIO] LỖI gắn chân I2S: 0x%X\n", err);
        return false;
    }

    // 3. Khởi tạo Codec ES8311 qua I2C nếu có trên mạch
    es8311_init_codec();
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

    // 5. Khởi tạo FreeRTOS Task chạy trên Core 0
    xTaskCreatePinnedToCore(
        audio_background_task,
        "Audio_Task",
        4096,
        NULL,
        2, // Priority
        &audio_task_handle,
        0  // Core 0 (để Core 1 chuyên cho LVGL Display)
    );

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
    if (!psram_record_buf) return false;

    audio_stop_playback();
    record_sample_capacity = AUDIO_SAMPLE_RATE * max_duration_sec;
    if (record_sample_capacity > AUDIO_MAX_SAMPLES)
    {
        record_sample_capacity = AUDIO_MAX_SAMPLES;
    }
    recorded_samples_count = 0;
    recording_active = true;
    Serial.printf("[AUDIO] Bắt đầu ghi âm Mic vào PSRAM (Tối đa %u giây)...\n", max_duration_sec);
    return true;
}

void audio_stop_recording(void)
{
    recording_active = false;
    Serial.printf("[AUDIO] Đã dừng ghi âm. Thu được %u mẫu (%.2f giây)\n",
                  recorded_samples_count, (float)recorded_samples_count / AUDIO_SAMPLE_RATE);
}

bool audio_is_recording(void)
{
    return recording_active;
}

bool audio_start_playback(void)
{
    if (!psram_record_buf || recorded_samples_count == 0) return false;

    audio_stop_recording();
    playback_sample_idx = 0;
    playback_active = true;
    Serial.printf("[AUDIO] Bắt đầu phát lại đoạn ghi âm (%u mẫu)...\n", recorded_samples_count);
    return true;
}

void audio_stop_playback(void)
{
    playback_active = false;
    playback_sample_idx = 0;
}

bool audio_is_playing(void)
{
    return playback_active;
}

uint32_t audio_get_recorded_duration_ms(void)
{
    return (recorded_samples_count * 1000) / AUDIO_SAMPLE_RATE;
}

uint32_t audio_get_playback_progress_ms(void)
{
    return (playback_sample_idx * 1000) / AUDIO_SAMPLE_RATE;
}

/* =========================================================================
 * DỮ LIỆU TELEMETRY MICROPHONE THỜI GIAN THỰC
 * ========================================================================= */
uint8_t audio_get_mic_level(void)
{
    return current_mic_level;
}

float audio_get_mic_db(void)
{
    return current_mic_db;
}

void audio_get_waveform_samples(int16_t *dest, size_t count)
{
    if (!dest || count == 0) return;
    if (count > 128) count = 128;

    size_t start = (waveform_head + 128 - count) % 128;
    for (size_t i = 0; i < count; i++)
    {
        dest[i] = waveform_history[(start + i) % 128];
    }
}
