/**
 * @file audio_manager.h
 * @brief Phân hệ điều khiển âm thanh I2S Duplex (Microphone MEMS & Loa ngoài FM8002E/ES8311)
 * Thiết kế cho bo mạch ES3C28P ESP32-S3 2.8" IPS (XiaoZhi AI Native)
 */

#pragma once

#include <Arduino.h>
#include "board_config.h"

// Định nghĩa chân phần cứng Audio (lấy từ board profile thông qua board_config.h)
#ifndef AUDIO_I2S_BCLK
#define AUDIO_I2S_BCLK      BOARD_AUDIO_I2S_BCLK
#endif
#ifndef AUDIO_I2S_WS
#define AUDIO_I2S_WS        BOARD_AUDIO_I2S_WS
#endif
#ifndef AUDIO_I2S_DOUT
#define AUDIO_I2S_DOUT      BOARD_AUDIO_I2S_DOUT
#endif
#ifndef AUDIO_I2S_DIN
#define AUDIO_I2S_DIN       BOARD_AUDIO_I2S_DIN
#endif
#ifndef AUDIO_I2S_MCLK
#define AUDIO_I2S_MCLK      BOARD_AUDIO_I2S_MCLK
#endif
#ifndef AUDIO_PA_PIN
#define AUDIO_PA_PIN        BOARD_AUDIO_PA_PIN
#endif
#ifndef AUDIO_I2C_SDA
#define AUDIO_I2C_SDA       BOARD_AUDIO_I2C_SDA
#endif
#ifndef AUDIO_I2C_SCL
#define AUDIO_I2C_SCL       BOARD_AUDIO_I2C_SCL
#endif
#ifndef AUDIO_ES8311_ADDR
#define AUDIO_ES8311_ADDR   BOARD_AUDIO_ES8311_ADDR
#endif

// Tần số lấy mẫu âm thanh chuẩn cho AI & Voice (16kHz, 16-bit Mono)
#define AUDIO_SAMPLE_RATE       16000
#define AUDIO_RECORD_MAX_SEC    10
#define AUDIO_MAX_SAMPLES       (AUDIO_SAMPLE_RATE * AUDIO_RECORD_MAX_SEC) // 160,000 samples (320,000 bytes)

enum SoundEffect
{
    FX_CLICK = 0,
    FX_BEEP,
    FX_CHIME,
    FX_MELODY,
    FX_XIAOZHI_WAKE
};

// Cơ chế phân quyền phần cứng I2S (I2S_NUM_0)
enum AudioOwner
{
    AUDIO_OWNER_NONE = 0,
    AUDIO_OWNER_SYSTEM,     // System tones, sound effects, audio lab
    AUDIO_OWNER_RECORDER,   // Mic input, AI voice input
    AUDIO_OWNER_MUSIC,      // ESP32-audioI2S playback
    AUDIO_OWNER_AI_VOICE    // AI voice speech synthesis playback
};

enum AudioRecordingFileState
{
    AUDIO_FILE_NONE = 0,
    AUDIO_FILE_SAVING,
    AUDIO_FILE_SAVED,
    AUDIO_FILE_ERROR
};

/**
 * @brief Yêu cầu quyền sở hữu phần cứng I2S
 * @param requester Phân hệ yêu cầu
 * @return true nếu được cấp quyền
 */
bool audio_request_ownership(AudioOwner requester);

/**
 * @brief Giải phóng quyền sở hữu phần cứng I2S
 * @param requester Phân hệ giải phóng
 */
void audio_release_ownership(AudioOwner requester);

/**
 * @brief Lấy chủ sở hữu phần cứng I2S hiện tại
 */
AudioOwner audio_get_current_owner(void);

/**
 * @brief Cài đặt Driver I2S Duplex (16kHz 16-bit Master TX+RX) cho hệ thống âm thanh nội bộ
 * @return true nếu driver được cài đặt thành công
 */
bool audio_install_duplex_driver(void);

/**
 * @brief Gỡ bỏ Driver I2S Duplex để nhường cổng I2S_NUM_0 hoàn toàn cho ESP32-audioI2S
 */
void audio_uninstall_duplex_driver(void);

/**
 * @brief Kiểm tra xem I2S Duplex driver của AudioManager có đang được cài đặt không
 */
bool audio_is_driver_installed(void);

/**
 * @brief Yêu cầu dừng an toàn tác vụ Audio Task nền và chờ xác nhận ACK trước khi gỡ driver
 * @param timeout_ms Thời gian chờ tối đa (ms)
 * @return true nếu Audio Task đã vào trạng thái PAUSED an toàn
 */
bool audio_manager_pause_task_sync(uint32_t timeout_ms = 300);

/**
 * @brief Đánh thức và khôi phục hoạt động cho tác vụ Audio Task nền sau khi cài đặt lại driver
 */
void audio_manager_resume_task(void);

/**
 * @brief Khởi tạo Driver I2S Duplex và cấu hình Codec ES8311 / PA Loa
 * Chạy tác vụ xử lý âm thanh ngầm trên Core 0 (đảm bảo không gián đoạn đồ họa LVGL trên Core 1)
 */
bool audio_manager_init(void);

/**
 * @brief Điều chỉnh âm lượng phát ra loa (0 - 100%)
 */
void audio_set_volume(uint8_t volume_percent);
uint8_t audio_get_volume(void);

/**
 * @brief Bật/Tắt IC khuếch đại công suất Loa (Power Amplifier)
 */
void audio_set_pa_enabled(bool enabled);
bool audio_is_pa_enabled(void);

/**
 * @brief Phát một âm sắc hình sin với tần số và thời lượng xác định
 */
void audio_play_tone(uint32_t freq_hz, uint32_t duration_ms);

/**
 * @brief Phát hiệu ứng âm thanh định sẵn
 */
void audio_play_sound_effect(SoundEffect fx);

/**
 * @brief Bắt đầu ghi âm trực tiếp từ Microphone vào bộ nhớ 8MB Octal PSRAM
 * @param max_duration_sec Thời gian ghi âm tối đa (mặc định 10 giây)
 */
bool audio_start_recording(uint32_t max_duration_sec = AUDIO_RECORD_MAX_SEC);
void audio_stop_recording(void);
/** Stop a recording and discard it without scheduling a WAV export. */
void audio_cancel_recording(void);
bool audio_is_recording(void);

/**
 * @brief Bắt đầu phát lại đoạn âm thanh vừa thu âm trong PSRAM ra loa
 */
bool audio_start_playback(void);
void audio_stop_playback(void);
bool audio_is_playing(void);

/**
 * @brief Lấy thời lượng âm thanh đã ghi (ms)
 */
uint32_t audio_get_recorded_duration_ms(void);

/**
 * @brief Lấy tiến trình đang phát lại (ms)
 */
uint32_t audio_get_playback_progress_ms(void);
size_t audio_get_recorded_sample_count(void);
size_t audio_copy_recorded_samples(size_t offset, int16_t *dest, size_t max_samples);
AudioRecordingFileState audio_get_recording_file_state(void);
const char *audio_get_recording_file_path(void);

/** Write mono PCM16 samples through the installed duplex driver. Caller owns I2S. */
bool audio_write_pcm16_mono(const int16_t *samples, size_t count, uint32_t timeout_ms = 100);

/**
 * @brief Lấy cường độ âm thanh thời gian thực từ Microphone (0 - 100%)
 */
uint8_t audio_get_mic_level(void);

/**
 * @brief Lấy chỉ số Decibel (dB) từ Microphone (-60dB đến 0dB)
 */
float audio_get_mic_db(void);

/**
 * @brief Lấy mẫu dạng sóng âm thanh từ Microphone để vẽ Oscilloscope / Waveform
 * @param dest Mảng nhận dữ liệu mẫu
 * @param count Số lượng mẫu cần lấy
 */
void audio_get_waveform_samples(int16_t *dest, size_t count);
