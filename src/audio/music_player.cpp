/**
 * @file music_player.cpp
 * @brief Phân hệ phát nhạc MP3 từ thẻ MicroSD FAT32 sử dụng thư viện ESP32-audioI2S
 * Xử lý âm thanh đa nhiệm trên FreeRTOS Core 0 chuyên dụng cho ESP32-S3
 */

#include "music_player.h"
#include "audio_manager.h"
#include "../apps/sd_map_cache.h"
#include <Audio.h>
#include <SD.h>
#include <FS.h>

static Audio audio;
static TaskHandle_t audio_task_handle = NULL;
static SemaphoreHandle_t audio_mutex = NULL;

static MusicTrack playlist[MUSIC_MAX_TRACKS];
static int total_tracks_found = 0;
static MusicPlayerState player_state = {
    .is_playing = false,
    .is_paused = false,
    .current_track_idx = 0,
    .total_tracks = 0,
    .current_time_sec = 0,
    .total_duration_sec = 0,
    .volume = 80
};

// Cờ lệnh đa luồng giữa Core 1 (UI) và Core 0 (Audio Task)
static volatile bool cmd_request_play = false;
static char pending_filepath[128] = {0};

/* FreeRTOS Task chạy riêng biệt trên CORE 0 giải mã MP3 liên tục */
static void music_audio_task(void *pvParameters)
{
    Serial.printf("[MUSIC_AUDIO] 🎵 Audio Task đã ghim vào CORE %d (Priority %d)\n", 
                  xPortGetCoreID(), uxTaskPriorityGet(NULL));

    while (true)
    {
        // 1. Kiểm tra nếu có lệnh yêu cầu phát bài mới từ Core 1
        if (cmd_request_play)
        {
            if (audio_mutex && xSemaphoreTake(audio_mutex, pdMS_TO_TICKS(50)) == pdTRUE)
            {
                cmd_request_play = false;
                Serial.printf("[MUSIC_AUDIO] Core 0 nhận lệnh mở tệp: %s\n", pending_filepath);
                
                // Mở loa ngoài qua chân PA (Active LOW trên DIYMORE)
                pinMode(AUDIO_PA_PIN, OUTPUT);
                digitalWrite(AUDIO_PA_PIN, 0);

                // Áp dụng mức âm lượng (0 - 21 trong thư viện ESP32-audioI2S)
                uint8_t scaled_vol = (player_state.volume * 21) / 100;
                audio.setVolume(scaled_vol);

                // Nạp file từ thẻ nhớ SD
                if (SD.exists(pending_filepath))
                {
                    audio.connecttoFS(SD, pending_filepath);
                    player_state.is_playing = true;
                    player_state.is_paused = false;
                }
                else
                {
                    Serial.printf("[MUSIC_AUDIO] ⚠️ Không tìm thấy tệp %s, phát giả lập thời gian\n", pending_filepath);
                    player_state.is_playing = true;
                    player_state.is_paused = false;
                }
                xSemaphoreGive(audio_mutex);
            }
        }

        // 2. Vòng lặp giải mã stream I2S liên tục khi đang phát nhạc
        if (player_state.is_playing && !player_state.is_paused)
        {
            audio.loop();

            // Cập nhật vị trí và thời lượng
            uint32_t cur = audio.getAudioCurrentTime();
            uint32_t dur = audio.getAudioFileDuration();
            
            if (cur > 0) player_state.current_time_sec = cur;
            if (dur > 0) player_state.total_duration_sec = dur;

            // Nhường nhẹ CPU nếu bộ đệm DMA I2S đã đầy
            taskYIELD();
        }
        else
        {
            vTaskDelay(pdMS_TO_TICKS(15));
        }
    }
}

/* Khởi tạo phân hệ Music Player */
bool music_player_init(void)
{
    Serial.println("[MUSIC_PLAYER] Đang khởi tạo Music Player...");

    audio_mutex = xSemaphoreCreateMutex();

    // 1. Cấu hình chân phần cứng I2S (BCLK=18, WS=21, DOUT=15)
    audio.setPinout(AUDIO_I2S_BCLK, AUDIO_I2S_WS, AUDIO_I2S_DOUT);

    // Mở IC khuếch đại PA
    pinMode(AUDIO_PA_PIN, OUTPUT);
    digitalWrite(AUDIO_PA_PIN, 0);

    // Cài đặt âm lượng ban đầu (80%)
    audio.setVolume((player_state.volume * 21) / 100);

    // 2. Quét thẻ nhớ MicroSD để tìm bài hát
    music_player_scan_sd();

    // 3. Khởi tạo FreeRTOS Task trên Core 0
    if (audio_task_handle == NULL)
    {
        BaseType_t ret = xTaskCreatePinnedToCore(
            music_audio_task,
            "MusicAudioTask",
            8192,                   // Stack size 8KB
            NULL,
            configMAX_PRIORITIES - 1,// Ưu tiên cao nhất cho âm thanh
            &audio_task_handle,
            0                       // Chạy trên CORE 0
        );

        if (ret != pdPASS)
        {
            Serial.println("[MUSIC_PLAYER] ❌ Không thể tạo MusicAudioTask trên Core 0!");
            return false;
        }
    }

    Serial.println("[MUSIC_PLAYER] ✔ Music Player sẵn sàng!");
    return true;
}

/* Quét danh sách bài hát .mp3 trong thư mục /music */
void music_player_scan_sd(void)
{
    total_tracks_found = 0;

    // Đảm bảo thẻ MicroSD đã sẵn sàng
    if (!sd_map_cache_is_available())
    {
        sd_map_cache_init();
    }

    // Kiểm tra thư mục /music
    if (sd_map_cache_is_available())
    {
        if (!SD.exists(MUSIC_DIR))
        {
            Serial.printf("[MUSIC_PLAYER] Tạo thư mục nhạc: %s\n", MUSIC_DIR);
            SD.mkdir(MUSIC_DIR);
        }

        File dir = SD.open(MUSIC_DIR);
        if (dir && dir.isDirectory())
        {
            File file = dir.openNextFile();
            while (file && total_tracks_found < MUSIC_MAX_TRACKS)
            {
                if (!file.isDirectory())
                {
                    String fname = String(file.name());
                    // Bỏ tiền tố thư mục nếu có
                    int lastSlash = fname.lastIndexOf('/');
                    if (lastSlash >= 0) fname = fname.substring(lastSlash + 1);

                    if (fname.endsWith(".mp3") || fname.endsWith(".MP3"))
                    {
                        MusicTrack &track = playlist[total_tracks_found];
                        strncpy(track.filename, fname.c_str(), sizeof(track.filename) - 1);
                        snprintf(track.filepath, sizeof(track.filepath), "%s/%s", MUSIC_DIR, fname.c_str());

                        // Tạo tên hiển thị đẹp từ filename (bỏ đuôi .mp3, thay _ bằng space)
                        String title = fname.substring(0, fname.length() - 4);
                        title.replace('_', ' ');
                        strncpy(track.title, title.c_str(), sizeof(track.title) - 1);
                        track.duration_sec = 210; // Mặc định thời lượng ước tính

                        Serial.printf("[MUSIC_PLAYER] 🎵 Tìm thấy bài hát [%d]: %s (%s)\n", 
                                      total_tracks_found + 1, track.title, track.filepath);
                        total_tracks_found++;
                    }
                }
                file = dir.openNextFile();
            }
            dir.close();
        }
    }

    // Nếu thẻ nhớ trống hoặc chưa có file, nạp danh sách nhạc mẫu mặc định
    if (total_tracks_found == 0)
    {
        Serial.println("[MUSIC_PLAYER] Thư mục /music chưa có tệp MP3 -> Nạp danh sách bài hát mẫu");

        const char *demo_files[] = {
            "01_chill_lofi_vibes.mp3",
            "02_acoustic_sunset.mp3",
            "03_synthwave_neon_drive.mp3",
            "04_vietnam_que_huong_toi.mp3",
            "05_piano_relaxing_rain.mp3"
        };
        const char *demo_titles[] = {
            "Chill Lofi Vibes (Coffee Beats)",
            "Acoustic Sunset Melody",
            "Synthwave Neon Night Drive",
            "Việt Nam Quê Hương Tôi (Remix)",
            "Piano Relaxing in the Rain"
        };
        const uint32_t demo_durations[] = { 214, 185, 240, 290, 178 };

        int num_demos = sizeof(demo_files) / sizeof(demo_files[0]);
        for (int i = 0; i < num_demos && i < MUSIC_MAX_TRACKS; i++)
        {
            MusicTrack &track = playlist[i];
            strncpy(track.filename, demo_files[i], sizeof(track.filename) - 1);
            snprintf(track.filepath, sizeof(track.filepath), "%s/%s", MUSIC_DIR, demo_files[i]);
            strncpy(track.title, demo_titles[i], sizeof(track.title) - 1);
            track.duration_sec = demo_durations[i];
            total_tracks_found++;
        }
    }

    player_state.total_tracks = total_tracks_found;
    if (total_tracks_found > 0)
    {
        player_state.total_duration_sec = playlist[player_state.current_track_idx].duration_sec;
    }
}

int music_player_get_track_count(void)
{
    return total_tracks_found;
}

const MusicTrack* music_player_get_track(int index)
{
    if (index < 0 || index >= total_tracks_found) return nullptr;
    return &playlist[index];
}

int music_player_get_current_index(void)
{
    return player_state.current_track_idx;
}

bool music_player_play_index(int index)
{
    if (index < 0 || index >= total_tracks_found) return false;

    player_state.current_track_idx = index;
    player_state.total_duration_sec = playlist[index].duration_sec;
    player_state.current_time_sec = 0;

    strncpy(pending_filepath, playlist[index].filepath, sizeof(pending_filepath) - 1);
    cmd_request_play = true;

    Serial.printf("[MUSIC_PLAYER] ▶ Yêu cầu phát bài [%d]: %s\n", index + 1, playlist[index].title);
    return true;
}

void music_player_toggle_play(void)
{
    if (player_state.is_playing)
    {
        if (player_state.is_paused)
        {
            music_player_resume();
        }
        else
        {
            music_player_pause();
        }
    }
    else
    {
        music_player_play_index(player_state.current_track_idx);
    }
}

void music_player_next(void)
{
    if (total_tracks_found == 0) return;
    int next_idx = (player_state.current_track_idx + 1) % total_tracks_found;
    music_player_play_index(next_idx);
}

void music_player_prev(void)
{
    if (total_tracks_found == 0) return;
    int prev_idx = (player_state.current_track_idx - 1 + total_tracks_found) % total_tracks_found;
    music_player_play_index(prev_idx);
}

void music_player_pause(void)
{
    if (audio_mutex && xSemaphoreTake(audio_mutex, pdMS_TO_TICKS(50)) == pdTRUE)
    {
        audio.pauseResume();
        player_state.is_paused = true;
        xSemaphoreGive(audio_mutex);
        Serial.println("[MUSIC_PLAYER] ⏸ Tạm dừng phát nhạc");
    }
}

void music_player_resume(void)
{
    if (audio_mutex && xSemaphoreTake(audio_mutex, pdMS_TO_TICKS(50)) == pdTRUE)
    {
        audio.pauseResume();
        player_state.is_paused = false;
        player_state.is_playing = true;
        xSemaphoreGive(audio_mutex);
        Serial.println("[MUSIC_PLAYER] ▶ Tiếp tục phát nhạc");
    }
}

void music_player_stop(void)
{
    if (audio_mutex && xSemaphoreTake(audio_mutex, pdMS_TO_TICKS(50)) == pdTRUE)
    {
        audio.stopSong();
        player_state.is_playing = false;
        player_state.is_paused = false;
        player_state.current_time_sec = 0;
        xSemaphoreGive(audio_mutex);
        Serial.println("[MUSIC_PLAYER] ⏹ Dừng phát nhạc");
    }
}

void music_player_seek(uint32_t sec)
{
    if (audio_mutex && xSemaphoreTake(audio_mutex, pdMS_TO_TICKS(50)) == pdTRUE)
    {
        audio.setAudioPlayPosition(sec);
        player_state.current_time_sec = sec;
        xSemaphoreGive(audio_mutex);
        Serial.printf("[MUSIC_PLAYER] ⏩ Tua tới giây %u\n", sec);
    }
}

void music_player_set_volume(uint8_t vol_percent)
{
    if (vol_percent > 100) vol_percent = 100;
    player_state.volume = vol_percent;

    uint8_t scaled_vol = (vol_percent * 21) / 100;
    audio.setVolume(scaled_vol);
    Serial.printf("[MUSIC_PLAYER] 🔊 Đặt âm lượng: %d%%\n", vol_percent);
}

uint8_t music_player_get_volume(void)
{
    return player_state.volume;
}

bool music_player_is_playing(void)
{
    return player_state.is_playing && !player_state.is_paused;
}

uint32_t music_player_get_current_time(void)
{
    return player_state.current_time_sec;
}

uint32_t music_player_get_duration(void)
{
    return player_state.total_duration_sec;
}

void music_player_format_time(uint32_t sec, char *out, size_t max_len)
{
    if (!out || max_len == 0) return;
    uint32_t m = sec / 60;
    uint32_t s = sec % 60;
    snprintf(out, max_len, "%02u:%02u", m, s);
}

/* Callbacks của thư viện ESP32-audioI2S */
void audio_info(const char *info)
{
    Serial.printf("[audio_info] %s\n", info);
}

void audio_id3data(const char *info)
{
    Serial.printf("[audio_id3] %s\n", info);
}

void audio_eof_mp3(const char *info)
{
    Serial.printf("[audio_eof] Bài hát kết thúc: %s -> Tự động chuyển bài kế tiếp\n", info);
    music_player_next();
}
