/**
 * @file music_player.h
 * @brief Phân hệ phát nhạc MP3 từ thẻ MicroSD FAT32 sử dụng thư viện ESP32-audioI2S
 * Xử lý âm thanh đa nhiệm trên FreeRTOS Core 0 chuyên dụng cho ESP32-S3
 */

#pragma once

#include <Arduino.h>

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
void music_player_toggle_play(void);

/**
 * @brief Chuyển sang bài hát tiếp theo
 */
void music_player_next(void);

/**
 * @brief Quay lại bài hát trước đó
 */
void music_player_prev(void);

/**
 * @brief Tạm dừng phát nhạc
 */
void music_player_pause(void);

/**
 * @brief Tiếp tục phát nhạc
 */
void music_player_resume(void);

/**
 * @brief Dừng phát nhạc
 */
void music_player_stop(void);

/**
 * @brief Tua thời lượng bài hát đến giây thứ sec
 */
void music_player_seek(uint32_t sec);

/**
 * @brief Cài đặt âm lượng phát ra loa (0 - 100%)
 */
void music_player_set_volume(uint8_t vol_percent);
uint8_t music_player_get_volume(void);

/**
 * @brief Kiểm tra trạng thái đang phát nhạc
 */
bool music_player_is_playing(void);

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
