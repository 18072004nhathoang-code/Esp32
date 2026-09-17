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
#include "service_state_logic.h"

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
static bool music_owns_audio = false;
static uint32_t music_owner_session = 0;
static uint32_t codec_sample_rate = 0;
static constexpr uint32_t MUSIC_EVENT_EOF = 1U << 0;
static constexpr uint32_t MUSIC_EVENT_STOP = 1U << 1;

static bool enqueue_music_command(const MusicCommand &cmd)
{
    if (!music_cmd_queue || xQueueSend(music_cmd_queue, &cmd, 0) != pdTRUE)
    {
        Serial.println("[MUSIC_PLAYER] ❌ Hàng đợi lệnh không sẵn sàng hoặc đã đầy");
        return false;
    }
    return true;
}

static bool acquire_music_audio(void)
{
    if (music_owns_audio) return true;
    if (!audio_request_ownership(AUDIO_OWNER_MUSIC)) return false;
    music_owner_session = audio_get_owner_session(AUDIO_OWNER_MUSIC);
    if (music_owner_session == 0)
    {
        audio_release_ownership(AUDIO_OWNER_MUSIC);
        return false;
    }
    music_owns_audio = true;
    return true;
}

static void release_music_audio(void)
{
    if (!music_owns_audio) return;
    music_owns_audio = false;
    const uint32_t session = music_owner_session;
    music_owner_session = 0;
    audio_release_ownership_session(AUDIO_OWNER_MUSIC, session);
}

/* Hàm hỗ trợ dừng và dọn dẹp Audio engine nội bộ trên Core 0 */
static void internal_stop_audio_locked(void)
{
    if (music_owns_audio)
        audio_set_pa_for_session(AUDIO_OWNER_MUSIC, music_owner_session, false);
    if (audio)
    {
        if (storage_lock(200))
        {
            audio->stopSong();
            storage_unlock();
        }
        audio_drain_tx(300);
        delete audio;
        audio = nullptr;
    }
    codec_sample_rate = 0;
    player_state.is_playing = false;
    player_state.is_paused = false;
    player_state.current_time_sec = 0;
}

static void handle_eof_event(void)
{
    int next_idx = -1;
    if (audio_mutex && xSemaphoreTake(audio_mutex, portMAX_DELAY) == pdTRUE)
    {
        next_idx = music_eof_next_index(player_state.is_playing,
                                        player_state.current_track_idx, total_tracks_found);
        if (next_idx >= 0)
        {
            internal_stop_audio_locked();
        }
        xSemaphoreGive(audio_mutex);
    }
    if (next_idx < 0) return;
    release_music_audio();
    if (!music_player_play_index(next_idx))
        Serial.println("[MUSIC_AUDIO] EOF cleanup complete; next command enqueue failed");
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
                        bool stopped = false;
                        if (audio_mutex && xSemaphoreTake(audio_mutex, pdMS_TO_TICKS(100)) == pdTRUE)
                        {
                            internal_stop_audio_locked();
                            stopped = true;
                            xSemaphoreGive(audio_mutex);
                        }
                        if (stopped) release_music_audio();
                    }
                    else
                    {
                        bool newly_acquired = !music_owns_audio;
                        if (acquire_music_audio())
                        {
                            if (audio_mutex && xSemaphoreTake(audio_mutex, pdMS_TO_TICKS(200)) == pdTRUE)
                            {
                                internal_stop_audio_locked();

                                audio = new Audio();
                                if (audio)
                                {
                                    const bool pins_ok = audio->setPinout(AUDIO_I2S_BCLK, AUDIO_I2S_WS,
                                                                         AUDIO_I2S_DOUT, AUDIO_I2S_MCLK);
                                    audio->setVolume(21); // unity in decoder; ES8311 owns user volume
                                    audio_set_volume(player_state.volume);

                                    // Kết nối FS với khóa bảo vệ storage (Thứ tự khóa: audio_mutex TRƯỚC, storage_lock SAU -> Zero Deadlock)
                                    bool connected = false;
                                    if (pins_ok && audio_codec_configure_for_stream(44100, 128) &&
                                        storage_lock(1000))
                                    {
                                        connected = audio->connecttoFS(storage_get_fs(), cmd.filepath);
                                        storage_unlock();
                                    }

                                    if (connected)
                                    {
                                        player_state.current_track_idx = idx;
                                        player_state.total_duration_sec = playlist[idx].duration_sec;
                                        player_state.current_time_sec = 0;
                                        player_state.is_playing = true;
                                        player_state.is_paused = false;
                                        codec_sample_rate = 44100;
                                        audio_set_pa_for_session(AUDIO_OWNER_MUSIC, music_owner_session, true);
                                        Serial.printf("[MUSIC_AUDIO] ▶ Bắt đầu phát nhạc: %s\n", cmd.filepath);
                                    }
                                    else
                                    {
                                        Serial.printf("[MUSIC_AUDIO] ❌ connecttoFS() thất bại cho tệp %s\n", cmd.filepath);
                                        internal_stop_audio_locked();
                                        release_music_audio();
                                    }
                                }
                                else
                                {
                                    release_music_audio();
                                }
                                xSemaphoreGive(audio_mutex);
                            }
                            else
                            {
                                if (newly_acquired) release_music_audio();
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
                            if (audio->pauseResume())
                            {
                                player_state.is_paused = true;
                                audio_set_pa_for_session(AUDIO_OWNER_MUSIC, music_owner_session, false);
                                Serial.println("[MUSIC_AUDIO] ⏸ Đã tạm dừng phát nhạc");
                            }
                        }
                        xSemaphoreGive(audio_mutex);
                    }
                }
                break;

                case MUSIC_CMD_RESUME:
                {
                    bool acquired_for_resume = !music_owns_audio;
                    if (acquire_music_audio())
                    {
                        bool resumed = false;
                        if (audio_mutex && xSemaphoreTake(audio_mutex, pdMS_TO_TICKS(100)) == pdTRUE)
                        {
                            if (audio && player_state.is_paused)
                            {
                                if (audio->pauseResume())
                                {
                                    player_state.is_paused = false;
                                    player_state.is_playing = true;
                                    resumed = true;
                                    audio_set_pa_for_session(AUDIO_OWNER_MUSIC, music_owner_session, true);
                                    Serial.println("[MUSIC_AUDIO] ▶ Đã tiếp tục phát nhạc");
                                }
                            }
                            xSemaphoreGive(audio_mutex);
                        }
                        if (acquired_for_resume && !resumed)
                        {
                            release_music_audio();
                        }
                    }
                }
                break;

                case MUSIC_CMD_SEEK:
                {
                    if (audio_mutex && xSemaphoreTake(audio_mutex, pdMS_TO_TICKS(100)) == pdTRUE)
                    {
                        bool seeked = false;
                        if (audio)
                        {
                            if (storage_lock(200))
                            {
                                seeked = audio->setAudioPlayPosition(cmd.param);
                                storage_unlock();
                            }
                        }
                        if (seeked)
                        {
                            player_state.current_time_sec = cmd.param;
                        }
                        xSemaphoreGive(audio_mutex);
                    }
                }
                break;

                case MUSIC_CMD_SET_VOLUME:
                {
                    if (audio_mutex && xSemaphoreTake(audio_mutex, pdMS_TO_TICKS(50)) == pdTRUE)
                    {
                        player_state.volume = (uint8_t)cmd.param;
                        audio_set_volume(player_state.volume);
                        if (audio && player_state.is_playing && !player_state.is_paused)
                            audio_set_pa_for_session(AUDIO_OWNER_MUSIC, music_owner_session, true);
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
                    bool release_after_loop = false;
                    if (audio != nullptr && player_state.is_playing && !player_state.is_paused)
                    {
                        // Khóa storage bảo vệ đọc SPI/SD trong suốt chu kỳ loop
                        if (storage_lock(50))
                        {
                            audio->loop();
                            storage_unlock();
                        }

                        const uint32_t decoded_rate = audio->getSampleRate();
                        if (decoded_rate >= 8000 && decoded_rate <= 96000 &&
                            decoded_rate != codec_sample_rate)
                        {
                            if (audio_codec_configure_for_stream(decoded_rate, 128))
                                codec_sample_rate = decoded_rate;
                            else
                            {
                                Serial.printf("[MUSIC_AUDIO] ❌ Codec clock rejected Fs=%u\n", decoded_rate);
                                internal_stop_audio_locked();
                                release_after_loop = true;
                            }
                        }

                        if (audio)
                        {
                            uint32_t cur = audio->getAudioCurrentTime();
                            uint32_t dur = audio->getAudioFileDuration();
                            if (cur > 0) player_state.current_time_sec = cur;
                            if (dur > 0) player_state.total_duration_sec = dur;
                        }
                    }
                    xSemaphoreGive(audio_mutex);
                    if (release_after_loop) release_music_audio();
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

        uint32_t events = 0;
        if (xTaskNotifyWait(0, UINT32_MAX, &events, 0) == pdTRUE)
        {
            if (events & MUSIC_EVENT_STOP)
            {
                bool stopped = false;
                if (audio_mutex && xSemaphoreTake(audio_mutex, portMAX_DELAY) == pdTRUE)
                {
                    internal_stop_audio_locked();
                    stopped = true;
                    xSemaphoreGive(audio_mutex);
                }
                if (stopped) release_music_audio();
            }
            if (events & MUSIC_EVENT_EOF) handle_eof_event();
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

    if (!audio_mutex || !music_cmd_queue)
    {
        Serial.println("[MUSIC_PLAYER] ❌ Chế độ suy giảm: không tạo được mutex/queue");
        return false;
    }

    // PA remains disabled until a decoder has opened a real stream.
    // PA remains owned by AudioManager and is only enabled by an active session.

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
                            track.duration_sec = 0; // Cập nhật từ decoder sau khi mở tệp thật

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

    if (total_tracks_found == 0)
        Serial.println("[MUSIC_PLAYER] Không có tệp MP3 thật trong /music; playlist để trống");

    player_state.total_tracks = total_tracks_found;
    if (total_tracks_found > 0)
    {
        player_state.total_duration_sec = playlist[player_state.current_track_idx].duration_sec;
    }
    else
    {
        player_state.current_track_idx = 0;
        player_state.total_duration_sec = 0;
        player_state.current_time_sec = 0;
        player_state.is_playing = false;
        player_state.is_paused = false;
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
    if (!audio_mutex || xSemaphoreTake(audio_mutex, pdMS_TO_TICKS(20)) != pdTRUE) return -1;
    const int value = player_state.current_track_idx;
    xSemaphoreGive(audio_mutex);
    return value;
}

bool music_player_play_index(int index)
{
    if (index < 0 || index >= total_tracks_found) return false;

    MusicCommand cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.type = MUSIC_CMD_PLAY_INDEX;
    cmd.track_idx = index;
    strncpy(cmd.filepath, playlist[index].filepath, sizeof(cmd.filepath) - 1);

    if (!enqueue_music_command(cmd)) return false;

    Serial.printf("[MUSIC_PLAYER] ▶ Đã gửi lệnh phát bài [%d]: %s\n", index + 1, playlist[index].title);
    return true;
}

bool music_player_toggle_play(void)
{
    bool playing = false;
    bool paused = false;
    int current = -1;
    if (!audio_mutex || xSemaphoreTake(audio_mutex, pdMS_TO_TICKS(20)) != pdTRUE) return false;
    playing = player_state.is_playing;
    paused = player_state.is_paused;
    current = player_state.current_track_idx;
    xSemaphoreGive(audio_mutex);
    if (playing)
    {
        if (paused)
        {
            return music_player_resume();
        }
        else
        {
            return music_player_pause();
        }
    }
    else
    {
        return music_player_play_index(current);
    }
}

bool music_player_next(void)
{
    if (total_tracks_found == 0) return false;
    const int current = music_player_get_current_index();
    if (current < 0) return false;
    int next_idx = (current + 1) % total_tracks_found;
    return music_player_play_index(next_idx);
}

bool music_player_prev(void)
{
    if (total_tracks_found == 0) return false;
    const int current = music_player_get_current_index();
    if (current < 0) return false;
    int prev_idx = (current - 1 + total_tracks_found) % total_tracks_found;
    return music_player_play_index(prev_idx);
}

bool music_player_pause(void)
{
    MusicCommand cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.type = MUSIC_CMD_PAUSE;
    if (!enqueue_music_command(cmd)) return false;
    Serial.println("[MUSIC_PLAYER] ⏸ Gửi lệnh tạm dừng phát nhạc");
    return true;
}

bool music_player_resume(void)
{
    MusicCommand cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.type = MUSIC_CMD_RESUME;
    if (!enqueue_music_command(cmd)) return false;
    Serial.println("[MUSIC_PLAYER] ▶ Gửi lệnh tiếp tục phát nhạc");
    return true;
}

bool music_player_stop(void)
{
    if (!audio_task_handle ||
        xTaskNotify(audio_task_handle, MUSIC_EVENT_STOP, eSetBits) != pdPASS) return false;
    Serial.println("[MUSIC_PLAYER] ⏹ Gửi lệnh dừng phát nhạc");
    return true;
}

bool music_player_seek(uint32_t sec)
{
    if (!audio_mutex || xSemaphoreTake(audio_mutex, pdMS_TO_TICKS(20)) != pdTRUE) return false;
    const uint32_t duration = player_state.total_duration_sec;
    const bool playing = player_state.is_playing;
    xSemaphoreGive(audio_mutex);
    if (!playing || duration == 0 || sec > duration) return false;
    MusicCommand cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.type = MUSIC_CMD_SEEK;
    cmd.param = sec;
    if (!enqueue_music_command(cmd)) return false;
    Serial.printf("[MUSIC_PLAYER] ⏩ Gửi lệnh tua tới giây %u\n", sec);
    return true;
}

bool music_player_set_volume(uint8_t vol_percent)
{
    if (vol_percent > 100) vol_percent = 100;
    MusicCommand cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.type = MUSIC_CMD_SET_VOLUME;
    cmd.param = vol_percent;
    if (!enqueue_music_command(cmd)) return false;
    Serial.printf("[MUSIC_PLAYER] 🔊 Đặt âm lượng: %d%%\n", vol_percent);
    return true;
}


uint8_t music_player_get_volume(void)
{
    if (!audio_mutex || xSemaphoreTake(audio_mutex, pdMS_TO_TICKS(20)) != pdTRUE) return 0;
    const uint8_t value = player_state.volume;
    xSemaphoreGive(audio_mutex);
    return value;
}

bool music_player_is_playing(void)
{
    if (!audio_mutex || xSemaphoreTake(audio_mutex, pdMS_TO_TICKS(20)) != pdTRUE) return false;
    const bool value = player_state.is_playing && !player_state.is_paused;
    xSemaphoreGive(audio_mutex);
    return value;
}

uint32_t music_player_get_current_time(void)
{
    if (!audio_mutex || xSemaphoreTake(audio_mutex, pdMS_TO_TICKS(20)) != pdTRUE) return 0;
    const uint32_t value = player_state.current_time_sec;
    xSemaphoreGive(audio_mutex);
    return value;
}

uint32_t music_player_get_duration(void)
{
    if (!audio_mutex || xSemaphoreTake(audio_mutex, pdMS_TO_TICKS(20)) != pdTRUE) return 0;
    const uint32_t value = player_state.total_duration_sec;
    xSemaphoreGive(audio_mutex);
    return value;
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
    Serial.printf("[audio_eof] Bài hát kết thúc: %s; queue EOF event\n", info);
    if (audio_task_handle)
        xTaskNotify(audio_task_handle, MUSIC_EVENT_EOF, eSetBits);
}
