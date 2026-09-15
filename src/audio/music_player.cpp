/**
 * @file music_player.cpp
 * @brief Phân hệ phát nhạc MP3 từ thẻ MicroSD FAT32 sử dụng thư viện ESP32-audioI2S
 * Xử lý âm thanh đa nhiệm trên FreeRTOS Core 0 chuyên dụng cho ESP32-S3
 */

#include "music_player.h"
#include "audio_manager.h"
#include "board_config.h"
#include "../storage/storage_manager.h"
#include <Audio.h>
#include <FS.h>

static Audio *audio = nullptr;
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

// Các lệnh điều khiển phát nhạc đa luồng gửi qua FreeRTOS Queue
enum MusicCmdType
{
    MUSIC_CMD_NONE = 0,
    MUSIC_CMD_PLAY_INDEX,
    MUSIC_CMD_PAUSE,
    MUSIC_CMD_RESUME,
    MUSIC_CMD_TOGGLE_PLAY,
    MUSIC_CMD_NEXT,
    MUSIC_CMD_PREV,
    MUSIC_CMD_STOP,
    MUSIC_CMD_SEEK,
    MUSIC_CMD_SET_VOLUME
};

struct MusicCommand
{
    MusicCmdType type;
    int track_idx;
    uint32_t param;
    char filepath[128];
};

static QueueHandle_t music_cmd_queue = nullptr;

/* Hàm hỗ trợ dừng và dọn dẹp Audio engine nội bộ trên Core 0 */
static void internal_stop_audio_locked(void)
{
    if (audio)
    {
        if (storage_lock(200))
        {
            audio->stopSong();
            storage_unlock();
        }
        delete audio;
        audio = nullptr;
    }
    player_state.is_playing = false;
    player_state.is_paused = false;
    player_state.current_time_sec = 0;
}

/* FreeRTOS Task chạy riêng biệt trên CORE 0 giải mã MP3 liên tục */
static void music_audio_task(void *pvParameters)
{
    Serial.printf("[MUSIC_AUDIO] 🎵 Audio Task đã ghim vào CORE %d (Priority %d)\n", 
                  xPortGetCoreID(), uxTaskPriorityGet(NULL));

    while (true)
    {
        // 1. Nhận và xử lý các lệnh từ FreeRTOS Queue theo thứ tự (không bao giờ bị race condition)
        MusicCommand cmd;
        while (music_cmd_queue && xQueueReceive(music_cmd_queue, &cmd, 0) == pdTRUE)
        {
            switch (cmd.type)
            {
                case MUSIC_CMD_PLAY_INDEX:
                {
                    int idx = cmd.track_idx;
                    if (idx < 0 || idx >= total_tracks_found) break;

                    player_state.current_track_idx = idx;
                    player_state.total_duration_sec = playlist[idx].duration_sec;
                    player_state.current_time_sec = 0;

                    // Kiểm tra file có tồn tại trên thẻ SD trước với khóa storage
                    bool file_exists = false;
                    if (storage_lock(500))
                    {
                        file_exists = storage_get_fs().exists(cmd.filepath);
                        storage_unlock();
                    }

                    if (!file_exists)
                    {
                        Serial.printf("[MUSIC_AUDIO] ❌ Tệp %s không tồn tại trên thẻ SD -> Hủy phát!\n", cmd.filepath);
                        if (audio_mutex && xSemaphoreTake(audio_mutex, pdMS_TO_TICKS(100)) == pdTRUE)
                        {
                            internal_stop_audio_locked();
                            xSemaphoreGive(audio_mutex);
                        }
                        audio_release_ownership(AUDIO_OWNER_MUSIC);
                    }
                    else
                    {
                        if (audio_request_ownership(AUDIO_OWNER_MUSIC))
                        {
                            if (audio_mutex && xSemaphoreTake(audio_mutex, pdMS_TO_TICKS(200)) == pdTRUE)
                            {
                                // Mở loa ngoài qua chân PA
                                pinMode(AUDIO_PA_PIN, OUTPUT);
                                digitalWrite(AUDIO_PA_PIN, 0);

                                internal_stop_audio_locked();

                                audio = new Audio();
                                if (audio)
                                {
                                    audio->setPinout(AUDIO_I2S_BCLK, AUDIO_I2S_WS, AUDIO_I2S_DOUT);
                                    uint8_t scaled_vol = (player_state.volume * 21) / 100;
                                    audio->setVolume(scaled_vol);

                                    // Kết nối FS với khóa bảo vệ storage (Thứ tự khóa: audio_mutex TRƯỚC, storage_lock SAU -> Zero Deadlock)
                                    bool connected = false;
                                    if (storage_lock(1000))
                                    {
                                        connected = audio->connecttoFS(storage_get_fs(), cmd.filepath);
                                        storage_unlock();
                                    }

                                    if (connected)
                                    {
                                        player_state.is_playing = true;
                                        player_state.is_paused = false;
                                        Serial.printf("[MUSIC_AUDIO] ▶ Bắt đầu phát nhạc: %s\n", cmd.filepath);
                                    }
                                    else
                                    {
                                        Serial.printf("[MUSIC_AUDIO] ❌ connecttoFS() thất bại cho tệp %s\n", cmd.filepath);
                                        internal_stop_audio_locked();
                                        audio_release_ownership(AUDIO_OWNER_MUSIC);
                                    }
                                }
                                else
                                {
                                    audio_release_ownership(AUDIO_OWNER_MUSIC);
                                }
                                xSemaphoreGive(audio_mutex);
                            }
                            else
                            {
                                audio_release_ownership(AUDIO_OWNER_MUSIC);
                            }
                        }
                        else
                        {
                            Serial.println("[MUSIC_AUDIO] ⚠️ Không lấy được quyền sở hữu I2S cho MUSIC");
                        }
                    }
                }
                break;

                case MUSIC_CMD_PAUSE:
                {
                    if (audio_mutex && xSemaphoreTake(audio_mutex, pdMS_TO_TICKS(100)) == pdTRUE)
                    {
                        if (audio && player_state.is_playing && !player_state.is_paused)
                        {
                            audio->pauseResume();
                            player_state.is_paused = true;
                            Serial.println("[MUSIC_AUDIO] ⏸ Đã tạm dừng phát nhạc");
                        }
                        xSemaphoreGive(audio_mutex);
                    }
                }
                break;

                case MUSIC_CMD_RESUME:
                {
                    if (audio_request_ownership(AUDIO_OWNER_MUSIC))
                    {
                        if (audio_mutex && xSemaphoreTake(audio_mutex, pdMS_TO_TICKS(100)) == pdTRUE)
                        {
                            if (audio && player_state.is_paused)
                            {
                                audio->pauseResume();
                                player_state.is_paused = false;
                                player_state.is_playing = true;
                                Serial.println("[MUSIC_AUDIO] ▶ Đã tiếp tục phát nhạc");
                            }
                            xSemaphoreGive(audio_mutex);
                        }
                    }
                }
                break;

                case MUSIC_CMD_STOP:
                {
                    if (audio_mutex && xSemaphoreTake(audio_mutex, pdMS_TO_TICKS(200)) == pdTRUE)
                    {
                        internal_stop_audio_locked();
                        xSemaphoreGive(audio_mutex);
                    }
                    audio_release_ownership(AUDIO_OWNER_MUSIC);
                    Serial.println("[MUSIC_AUDIO] ⏹ Đã dừng phát nhạc");
                }
                break;

                case MUSIC_CMD_SEEK:
                {
                    if (audio_mutex && xSemaphoreTake(audio_mutex, pdMS_TO_TICKS(100)) == pdTRUE)
                    {
                        if (audio)
                        {
                            if (storage_lock(200))
                            {
                                audio->setAudioPlayPosition(cmd.param);
                                storage_unlock();
                            }
                        }
                        player_state.current_time_sec = cmd.param;
                        xSemaphoreGive(audio_mutex);
                    }
                }
                break;

                case MUSIC_CMD_SET_VOLUME:
                {
                    if (audio_mutex && xSemaphoreTake(audio_mutex, pdMS_TO_TICKS(50)) == pdTRUE)
                    {
                        if (audio)
                        {
                            uint8_t scaled_vol = (cmd.param * 21) / 100;
                            audio->setVolume(scaled_vol);
                        }
                        xSemaphoreGive(audio_mutex);
                    }
                }
                break;

                default:
                    break;
            }
        }

        // 2. Vòng lặp giải mã stream I2S liên tục khi đang phát nhạc
        if (player_state.is_playing && !player_state.is_paused)
        {
            if (audio_get_current_owner() == AUDIO_OWNER_MUSIC)
            {
                if (audio_mutex && xSemaphoreTake(audio_mutex, pdMS_TO_TICKS(20)) == pdTRUE)
                {
                    if (audio != nullptr && player_state.is_playing && !player_state.is_paused)
                    {
                        // Khóa storage bảo vệ đọc SPI/SD trong suốt chu kỳ loop
                        if (storage_lock(50))
                        {
                            audio->loop();
                            storage_unlock();
                        }

                        uint32_t cur = audio->getAudioCurrentTime();
                        uint32_t dur = audio->getAudioFileDuration();

                        if (cur > 0) player_state.current_time_sec = cur;
                        if (dur > 0) player_state.total_duration_sec = dur;
                    }
                    xSemaphoreGive(audio_mutex);
                }

                // Nhường nhẹ CPU để không làm đói các task khác trên Core 0
                vTaskDelay(pdMS_TO_TICKS(2));
            }
            else
            {
                // Nhường quyền cho task khác
                vTaskDelay(pdMS_TO_TICKS(20));
            }
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

    if (!audio_mutex)
    {
        audio_mutex = xSemaphoreCreateMutex();
    }
    if (!music_cmd_queue)
    {
        music_cmd_queue = xQueueCreate(16, sizeof(MusicCommand));
    }

    // Mở IC khuếch đại PA
    pinMode(AUDIO_PA_PIN, OUTPUT);
    digitalWrite(AUDIO_PA_PIN, 0);

    // Quét thẻ nhớ MicroSD để tìm bài hát (có khóa SPI bus)
    music_player_scan_sd();

    // Khởi tạo FreeRTOS Task trên Core 0 (Priority 3: Audio Realtime)
    if (audio_task_handle == NULL)
    {
        BaseType_t ret = xTaskCreatePinnedToCore(
            music_audio_task,
            "MusicAudioTask",
            8192,                   // Stack size 8KB
            NULL,
            3,                      // Priority 3: Audio Realtime (tránh CPU starvation)
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
    if (!storage_is_available())
    {
        storage_init();
    }

    // Kiểm tra thư mục /music với khóa bảo vệ bus
    if (storage_is_available())
    {
        if (storage_lock(1000))
        {
            if (!storage_get_fs().exists(MUSIC_DIR))
            {
                Serial.printf("[MUSIC_PLAYER] Tạo thư mục nhạc: %s\n", MUSIC_DIR);
                storage_get_fs().mkdir(MUSIC_DIR);
            }

            File dir = storage_get_fs().open(MUSIC_DIR);
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
            storage_unlock();
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

    MusicCommand cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.type = MUSIC_CMD_PLAY_INDEX;
    cmd.track_idx = index;
    strncpy(cmd.filepath, playlist[index].filepath, sizeof(cmd.filepath) - 1);

    if (music_cmd_queue)
    {
        xQueueSend(music_cmd_queue, &cmd, pdMS_TO_TICKS(50));
    }

    Serial.printf("[MUSIC_PLAYER] ▶ Đã gửi lệnh phát bài [%d]: %s\n", index + 1, playlist[index].title);
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
    MusicCommand cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.type = MUSIC_CMD_PAUSE;
    if (music_cmd_queue)
    {
        xQueueSend(music_cmd_queue, &cmd, pdMS_TO_TICKS(50));
    }
    player_state.is_paused = true;
    Serial.println("[MUSIC_PLAYER] ⏸ Gửi lệnh tạm dừng phát nhạc");
}

void music_player_resume(void)
{
    MusicCommand cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.type = MUSIC_CMD_RESUME;
    if (music_cmd_queue)
    {
        xQueueSend(music_cmd_queue, &cmd, pdMS_TO_TICKS(50));
    }
    player_state.is_paused = false;
    Serial.println("[MUSIC_PLAYER] ▶ Gửi lệnh tiếp tục phát nhạc");
}

void music_player_stop(void)
{
    MusicCommand cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.type = MUSIC_CMD_STOP;
    if (music_cmd_queue)
    {
        xQueueSend(music_cmd_queue, &cmd, pdMS_TO_TICKS(50));
    }
    player_state.is_playing = false;
    player_state.is_paused = false;
    Serial.println("[MUSIC_PLAYER] ⏹ Gửi lệnh dừng phát nhạc");
}

void music_player_seek(uint32_t sec)
{
    MusicCommand cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.type = MUSIC_CMD_SEEK;
    cmd.param = sec;
    if (music_cmd_queue)
    {
        xQueueSend(music_cmd_queue, &cmd, pdMS_TO_TICKS(50));
    }
    player_state.current_time_sec = sec;
    Serial.printf("[MUSIC_PLAYER] ⏩ Gửi lệnh tua tới giây %u\n", sec);
}

void music_player_set_volume(uint8_t vol_percent)
{
    if (vol_percent > 100) vol_percent = 100;
    player_state.volume = vol_percent;

    MusicCommand cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.type = MUSIC_CMD_SET_VOLUME;
    cmd.param = vol_percent;
    if (music_cmd_queue)
    {
        xQueueSend(music_cmd_queue, &cmd, pdMS_TO_TICKS(50));
    }
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
