/**
 * @file music_player.h
 * @brief Phân hệ phát nhạc MP3 từ thẻ MicroSD FAT32 sử dụng thư viện ESP32-audioI2S
 * Xử lý âm thanh đa nhiệm trên FreeRTOS Core 0 chuyên dụng cho ESP32-S3
 */

#pragma once

#include <Arduino.h>
#include "ai_voice_protocol.h"

#define MUSIC_DIR           "/music"
#define MUSIC_MAX_TRACKS    64

struct MusicTrack
{
    char filename[64];
    char filepath[128];
    char title[64];
    uint32_t duration_sec;
};

struct MusicPlayerState
{
    bool is_playing;
    bool is_paused;
    int current_track_idx;
    int total_tracks;
    uint32_t current_time_sec;
    uint32_t total_duration_sec;
    uint8_t volume; // 0 - 100%
};

struct MusicVoiceHandoff
{
    bool valid;
    bool resume_after_voice;
    bool is_stream;
    int track_index;
    uint32_t position_sec;
    char source_id[32];
};

/**
 * @brief Khởi tạo Driver I2S, Codec và khởi động Task FreeRTOS phát nhạc trên Core 0
 */
bool music_player_init(void);

/**
 * @brief Quét thẻ nhớ MicroSD để nạp danh sách các tệp .mp3 trong thư mục /music
 */
void music_player_scan_sd(void);

/**
 * @brief Lấy số lượng bài hát tìm thấy trong thư mục /music
 */
int music_player_get_track_count(void);

/**
 * @brief Lấy thông tin bài hát theo chỉ số index
 */
const MusicTrack* music_player_get_track(int index);

/**
 * @brief Lấy chỉ số bài hát đang được chọn / phát
 */
int music_player_get_current_index(void);

/**
 * @brief Bắt đầu phát bài hát theo chỉ số index
 */
bool music_player_play_index(int index);

/**
 * @brief Chuyển đổi trạng thái Play / Pause
 */
bool music_player_toggle_play(void);

/**
 * @brief Chuyển sang bài hát tiếp theo
 */
bool music_player_next(void);

/**
 * @brief Quay lại bài hát trước đó
 */
bool music_player_prev(void);

/**
 * @brief Tạm dừng phát nhạc
 */
bool music_player_pause(void);

/**
 * @brief Tiếp tục phát nhạc
 */
bool music_player_resume(void);

/**
 * @brief Dừng phát nhạc
 */
bool music_player_stop(void);

/**
 * @brief Tua thời lượng bài hát đến giây thứ sec
 */
bool music_player_seek(uint32_t sec);

/**
 * @brief Cài đặt âm lượng phát ra loa (0 - 100%)
 */
bool music_player_set_volume(uint8_t vol_percent);
uint8_t music_player_get_volume(void);

/**
 * @brief Kiểm tra trạng thái đang phát nhạc
 */
bool music_player_is_playing(void);
bool music_player_is_paused(void);

/** Execute an allowlisted AI action and wait for the decoder task ACK. */
bool music_player_execute_ai_action(const AiMusicAction *action, uint32_t timeout_ms,
                                    char *error, size_t error_size);

/** Fully destroy the decoder and release I2S before voice capture. */
bool music_player_suspend_for_voice(MusicVoiceHandoff *handoff, uint32_t timeout_ms,
                                    char *error, size_t error_size);

/** Restore a voice-suspended source after I2S has returned to MUSIC. */
bool music_player_restore_after_voice(const MusicVoiceHandoff *handoff, uint32_t timeout_ms,
                                      char *error, size_t error_size);

/**
 * @brief Lấy thời gian phát hiện tại (giây)
 */
uint32_t music_player_get_current_time(void);

/**
 * @brief Lấy tổng thời lượng bài hát (giây)
 */
uint32_t music_player_get_duration(void);

/**
 * @brief Định dạng thời gian giây thành chuỗi mm:ss
 */
void music_player_format_time(uint32_t sec, char *out, size_t max_len);
