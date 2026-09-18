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
#include <new>
#include <ArduinoJson.h>
#include "service_state_logic.h"
#include "music_decoder_lifecycle.h"

#if __has_include("secrets.h")
#include "secrets.h"
#endif
#ifndef AI_MUSIC_STREAM_SOURCES_JSON
#define AI_MUSIC_STREAM_SOURCES_JSON "[]"
#endif
#ifndef AI_MUSIC_STREAM_CA_CERT
#define AI_MUSIC_STREAM_CA_CERT ""
#endif

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
    MUSIC_CMD_SET_VOLUME,
    MUSIC_CMD_STOP,
    MUSIC_CMD_PLAY_STREAM
};

struct MusicCommand
{
    MusicCmdType type;
    int track_idx;
    uint32_t param;
    uint32_t request_id;
    char filepath[256];
    char source_id[32];
};

struct MusicCommandAck
{
    uint32_t request_id;
    bool ok;
};

struct MusicStreamSource
{
    char id[32];
    char url[256];
};

static QueueHandle_t music_cmd_queue = nullptr;
static QueueHandle_t music_ack_queue = nullptr;
static SemaphoreHandle_t music_ai_action_mutex = nullptr;
static MusicStreamSource stream_sources[8] = {};
static size_t stream_source_count = 0;
static uint32_t next_music_request_id = 0;
static bool music_owns_audio = false;
static uint32_t music_owner_session = 0;
static uint32_t codec_sample_rate = 0;
static MusicDecoderLifecycle decoder_lifecycle;
static uint32_t eof_generation = 0;
static constexpr uint32_t MUSIC_EVENT_EOF = 1U << 0;
static portMUX_TYPE music_control_mux = portMUX_INITIALIZER_UNLOCKED;
static bool music_stop_pending = false;
static uint32_t music_stop_session = 0;
static uint32_t music_stop_generation = 0;
static bool internal_stop_audio_locked(void);

static void complete_music_command(const MusicCommand &cmd, bool ok)
{
    if (!cmd.request_id || !music_ack_queue) return;
    const MusicCommandAck ack = {cmd.request_id, ok};
    if (xQueueSend(music_ack_queue, &ack, 0) != pdTRUE)
        Serial.println("[MUSIC_PLAYER] ACK queue full");
}

static void load_stream_sources(void)
{
    stream_source_count = 0;
    StaticJsonDocument<3072> document;
    if (deserializeJson(document, AI_MUSIC_STREAM_SOURCES_JSON) || !document.is<JsonArray>())
    {
        Serial.println("[MUSIC_PLAYER] Invalid AI_MUSIC_STREAM_SOURCES_JSON");
        return;
    }
    for (JsonObject source : document.as<JsonArray>())
    {
        if (stream_source_count >= 8) break;
        const char *id = source["id"] | "";
        const char *url = source["url"] | "";
        const size_t id_length = strlen(id);
        if (id_length == 0 || id_length >= sizeof(stream_sources[0].id) ||
            strncmp(url, "https://", 8) != 0 || strlen(url) >= sizeof(stream_sources[0].url))
        {
            Serial.println("[MUSIC_PLAYER] Ignoring invalid/non-HTTPS stream source");
            continue;
        }
        bool duplicate = false;
        for (size_t i = 0; i < stream_source_count; ++i)
            if (strcmp(stream_sources[i].id, id) == 0) duplicate = true;
        if (duplicate) continue;
        strlcpy(stream_sources[stream_source_count].id, id, sizeof(stream_sources[0].id));
        strlcpy(stream_sources[stream_source_count].url, url, sizeof(stream_sources[0].url));
        ++stream_source_count;
    }
}

static const MusicStreamSource *find_stream_source(const char *id)
{
    if (!id || !*id) return nullptr;
    for (size_t i = 0; i < stream_source_count; ++i)
        if (strcmp(stream_sources[i].id, id) == 0) return &stream_sources[i];
    return nullptr;
}

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

static bool release_music_audio(void)
{
    if (!music_owns_audio) return true;
    const uint32_t session = music_owner_session;
    if (!audio_release_ownership_session(AUDIO_OWNER_MUSIC, session))
    {
        Serial.println("[MUSIC_AUDIO] Duplex restore pending; MUSIC ownership retained for retry");
        return false;
    }
    music_owns_audio = false;
    music_owner_session = 0;
    return true;
}

static bool take_music_stop(uint32_t *session, uint32_t *generation)
{
    portENTER_CRITICAL(&music_control_mux);
    const bool pending = music_stop_pending;
    if (pending)
    {
        if (session) *session = music_stop_session;
        if (generation) *generation = music_stop_generation;
        music_stop_pending = false;
    }
    portEXIT_CRITICAL(&music_control_mux);
    return pending;
}

static void apply_music_stop(uint32_t target_session, uint32_t target_generation)
{
    const uint32_t current_session = music_owner_session;
    if (target_session != 0 && current_session != target_session) return;
    if (target_generation != 0 && decoder_lifecycle.generation() != target_generation) return;
    bool stopped = false;
    if (audio_mutex && xSemaphoreTake(audio_mutex, portMAX_DELAY) == pdTRUE)
    {
        if (target_session == 0 || music_owner_session == target_session)
        {
            stopped = internal_stop_audio_locked();
        }
        xSemaphoreGive(audio_mutex);
    }
    if (stopped) (void)release_music_audio();
}

/* Hàm hỗ trợ dừng và dọn dẹp Audio engine nội bộ trên Core 0 */
static bool internal_stop_audio_locked(void)
{
    const uint32_t generation = decoder_lifecycle.generation();
    if (!decoder_lifecycle.begin_stop(generation)) return false;
    player_state.is_playing = false;
    player_state.is_paused = false;
    if (music_owns_audio)
        audio_set_pa_for_session(AUDIO_OWNER_MUSIC, music_owner_session, false);
    if (audio)
    {
        if (!storage_lock(200))
        {
            (void)decoder_lifecycle.finish_stop(generation, false);
            Serial.println("[MUSIC_AUDIO] Stop deferred: storage lock unavailable");
            return false;
        }
        const bool shutdown_ack = audio->shutdown(1000);
        storage_unlock();
        if (!shutdown_ack)
        {
            (void)decoder_lifecycle.finish_stop(generation, false);
            Serial.println("[MUSIC_AUDIO] Stop timeout: decoder retained for safe recovery");
            return false;
        }
        (void)decoder_lifecycle.finish_stop(generation, true);
        audio_drain_tx(300);
        delete audio;
        audio = nullptr;
    }
    else
    {
        (void)decoder_lifecycle.finish_stop(generation, true);
    }
    codec_sample_rate = 0;
    player_state.is_playing = false;
    player_state.is_paused = false;
    player_state.current_time_sec = 0;
    return true;
}

static void handle_eof_event(void)
{
    int next_idx = -1;
    uint32_t event_generation = 0;
    portENTER_CRITICAL(&music_control_mux);
    event_generation = eof_generation;
    portEXIT_CRITICAL(&music_control_mux);
    if (audio_mutex && xSemaphoreTake(audio_mutex, portMAX_DELAY) == pdTRUE)
    {
        if (decoder_lifecycle.accepts_event(event_generation))
            next_idx = music_eof_next_index(player_state.is_playing,
                                            player_state.current_track_idx, total_tracks_found);
        if (next_idx >= 0)
        {
            if (!internal_stop_audio_locked()) next_idx = -1;
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

    uint32_t last_stack_report_ms = 0;
    while (true)
    {
        const uint32_t now_ms = millis();
        if (now_ms - last_stack_report_ms >= 30000U)
        {
            last_stack_report_ms = now_ms;
            Serial.printf("[MUSIC_AUDIO][STACK] MusicAudioTask high-water=%u bytes\n",
                          static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
            if (audio)
                Serial.printf("[MUSIC_AUDIO][STACK] PeriodicTask high-water=%u bytes\n",
                              static_cast<unsigned>(audio->getAudioTaskStackHighWaterMark()));
        }
        uint32_t stop_session = 0;
        uint32_t stop_generation = 0;
        if (take_music_stop(&stop_session, &stop_generation))
            apply_music_stop(stop_session, stop_generation);
        // 1. Nhận và xử lý các lệnh từ FreeRTOS Queue theo thứ tự (không bao giờ bị race condition)
        MusicCommand cmd;
        while (music_cmd_queue && xQueueReceive(music_cmd_queue, &cmd, 0) == pdTRUE)
        {
            bool command_ok = false;
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
                            stopped = internal_stop_audio_locked();
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
                                const bool prior_stopped = internal_stop_audio_locked();
                                if (!prior_stopped)
                                {
                                    Serial.println("[MUSIC_AUDIO] Play FAILED: decoder recovery required");
                                    xSemaphoreGive(audio_mutex);
                                    break;
                                }

                                portENTER_CRITICAL(&music_control_mux);
                                const uint32_t starting_generation = decoder_lifecycle.begin_start();
                                portEXIT_CRITICAL(&music_control_mux);
                                if (starting_generation == 0)
                                {
                                    Serial.println("[MUSIC_AUDIO] Play FAILED: lifecycle busy");
                                    xSemaphoreGive(audio_mutex);
                                    break;
                                }
                                audio = new(std::nothrow) Audio();
                                if (audio && audio->isInitialized())
                                {
                                    Serial.printf("[MUSIC_AUDIO][STACK] PeriodicTask high-water=%u bytes\n",
                                                  static_cast<unsigned>(audio->getAudioTaskStackHighWaterMark()));
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
                                        (void)decoder_lifecycle.finish_start(starting_generation, true);
                                        audio_set_pa_for_session(AUDIO_OWNER_MUSIC, music_owner_session, true);
                                        Serial.printf("[MUSIC_AUDIO] ▶ Bắt đầu phát nhạc: %s\n", cmd.filepath);
                                        command_ok = true;
                                    }
                                    else
                                    {
                                        Serial.printf("[MUSIC_AUDIO] ❌ connecttoFS() thất bại cho tệp %s\n", cmd.filepath);
                                        if (internal_stop_audio_locked()) release_music_audio();
                                    }
                                }
                                else
                                {
                                    Serial.println("[MUSIC_AUDIO] Play FAILED: Audio allocation/init");
                                    if (audio)
                                    {
                                        delete audio;
                                        audio = nullptr;
                                    }
                                    (void)decoder_lifecycle.finish_start(starting_generation, false);
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
                    bool release_after_pause = false;
                    if (audio_mutex && xSemaphoreTake(audio_mutex, pdMS_TO_TICKS(100)) == pdTRUE)
                    {
                        if (audio && player_state.is_playing && player_state.is_paused)
                            command_ok = true;
                        else if (audio && player_state.is_playing && !player_state.is_paused)
                        {
                            if (audio->pauseResume())
                            {
                                player_state.is_paused = true;
                                audio_set_pa_for_session(AUDIO_OWNER_MUSIC, music_owner_session, false);
                                release_after_pause = true;
                                command_ok = true;
                                Serial.println("[MUSIC_AUDIO] ⏸ Đã tạm dừng phát nhạc");
                            }
                        }
                        xSemaphoreGive(audio_mutex);
                    }
                    if (release_after_pause) command_ok = release_music_audio();
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
                            else if (audio && player_state.is_playing) resumed = true;
                            xSemaphoreGive(audio_mutex);
                        }
                        if (acquired_for_resume && !resumed)
                        {
                            release_music_audio();
                        }
                        command_ok = resumed;
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
                        command_ok = seeked;
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
                        command_ok = true;
                        xSemaphoreGive(audio_mutex);
                    }
                }
                break;

                case MUSIC_CMD_STOP:
                {
                    bool stopped = false;
                    if (audio_mutex && xSemaphoreTake(audio_mutex, pdMS_TO_TICKS(1000)) == pdTRUE)
                    {
                        stopped = internal_stop_audio_locked();
                        xSemaphoreGive(audio_mutex);
                    }
                    command_ok = stopped && release_music_audio();
                }
                break;

                case MUSIC_CMD_PLAY_STREAM:
                {
                    if (AI_MUSIC_STREAM_CA_CERT[0] == '\0' || strncmp(cmd.filepath, "https://", 8) != 0)
                        break;
                    const bool newly_acquired = !music_owns_audio;
                    if (!acquire_music_audio()) break;
                    if (audio_mutex && xSemaphoreTake(audio_mutex, pdMS_TO_TICKS(200)) == pdTRUE)
                    {
                        if (internal_stop_audio_locked())
                        {
                            const uint32_t generation = decoder_lifecycle.begin_start();
                            audio = new(std::nothrow) Audio();
                            if (generation && audio && audio->isInitialized())
                            {
                                const bool pins_ok = audio->setPinout(AUDIO_I2S_BCLK, AUDIO_I2S_WS,
                                                                     AUDIO_I2S_DOUT, AUDIO_I2S_MCLK);
                                audio->setCACert(AI_MUSIC_STREAM_CA_CERT);
                                audio->setVolume(21);
                                audio_set_volume(player_state.volume);
                                if (pins_ok && audio_codec_configure_for_stream(44100, 128) &&
                                    audio->connecttohost(cmd.filepath))
                                {
                                    player_state.current_track_idx = -1;
                                    player_state.current_time_sec = 0;
                                    player_state.total_duration_sec = 0;
                                    player_state.is_playing = true;
                                    player_state.is_paused = false;
                                    codec_sample_rate = 44100;
                                    (void)decoder_lifecycle.finish_start(generation, true);
                                    audio_set_pa_for_session(AUDIO_OWNER_MUSIC, music_owner_session, true);
                                    command_ok = true;
                                    Serial.printf("[MUSIC_AUDIO] ▶ HTTPS stream source: %s\n", cmd.source_id);
                                }
                            }
                            if (!command_ok)
                            {
                                if (audio) { delete audio; audio = nullptr; }
                                (void)decoder_lifecycle.finish_start(generation, false);
                            }
                        }
                        xSemaphoreGive(audio_mutex);
                    }
                    if (!command_ok && newly_acquired) release_music_audio();
                }
                break;

                default:
                    break;
            }
            complete_music_command(cmd, command_ok);
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
                                release_after_loop = internal_stop_audio_locked();
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
            static uint32_t last_recovery_ms = 0;
            bool decoder_stopped = decoder_lifecycle.can_destroy();
            if (decoder_lifecycle.phase() == MusicDecoderPhase::RECOVERY_REQUIRED &&
                millis() - last_recovery_ms >= 1000)
            {
                last_recovery_ms = millis();
                if (audio_mutex && xSemaphoreTake(audio_mutex, pdMS_TO_TICKS(100)) == pdTRUE)
                {
                    decoder_stopped = internal_stop_audio_locked();
                    xSemaphoreGive(audio_mutex);
                }
            }
            // Ownership is released only after decoder ACK and destruction.
            if (decoder_stopped && music_owns_audio && !player_state.is_paused)
                (void)release_music_audio();
            vTaskDelay(pdMS_TO_TICKS(20));
        }

        uint32_t events = 0;
        if (xTaskNotifyWait(0, UINT32_MAX, &events, 0) == pdTRUE)
        {
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

    if (!music_ack_queue) music_ack_queue = xQueueCreate(8, sizeof(MusicCommandAck));
    if (!music_ai_action_mutex) music_ai_action_mutex = xSemaphoreCreateMutex();

    if (!audio_mutex || !music_cmd_queue || !music_ack_queue || !music_ai_action_mutex)
    {
        Serial.println("[MUSIC_PLAYER] ❌ Chế độ suy giảm: không tạo được mutex/queue");
        return false;
    }

    // PA remains disabled until a decoder has opened a real stream.
    // PA remains owned by AudioManager and is only enabled by an active session.

    // Quét thẻ nhớ MicroSD để tìm bài hát (có khóa SPI bus)
    load_stream_sources();
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
    if (!audio_task_handle) return false;
    const uint32_t session = audio_get_owner_session(AUDIO_OWNER_MUSIC);
    portENTER_CRITICAL(&music_control_mux);
    music_stop_session = session;
    music_stop_generation = decoder_lifecycle.generation();
    music_stop_pending = true;
    portEXIT_CRITICAL(&music_control_mux);
    xTaskNotify(audio_task_handle, 0, eNoAction);
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
    portENTER_CRITICAL(&music_control_mux);
    eof_generation = decoder_lifecycle.generation();
    portEXIT_CRITICAL(&music_control_mux);
    if (audio_task_handle)
        xTaskNotify(audio_task_handle, MUSIC_EVENT_EOF, eSetBits);
}

bool music_player_is_paused(void)
{
    if (!audio_mutex || xSemaphoreTake(audio_mutex, pdMS_TO_TICKS(20)) != pdTRUE) return false;
    const bool value = player_state.is_playing && player_state.is_paused;
    xSemaphoreGive(audio_mutex);
    return value;
}

bool music_player_execute_ai_action(const AiMusicAction *action, uint32_t timeout_ms,
                                    char *error, size_t error_size)
{
    if (error && error_size) error[0] = '\0';
    if (!action || !ai_music_action_valid(*action) || !music_ai_action_mutex ||
        xSemaphoreTake(music_ai_action_mutex, pdMS_TO_TICKS(timeout_ms)) != pdTRUE)
    {
        if (error && error_size) strlcpy(error, "Lệnh nhạc không hợp lệ hoặc đang bận", error_size);
        return false;
    }
    MusicCommand cmd = {};
    cmd.request_id = ++next_music_request_id;
    switch (action->type)
    {
        case AI_MUSIC_ACTION_PLAY:
            if (action->source_id[0])
            {
                const MusicStreamSource *source = find_stream_source(action->source_id);
                if (!source)
                {
                    if (error && error_size) strlcpy(error, "Nguồn nhạc chưa được cấu hình trên thiết bị", error_size);
                    xSemaphoreGive(music_ai_action_mutex);
                    return false;
                }
                cmd.type = MUSIC_CMD_PLAY_STREAM;
                strlcpy(cmd.filepath, source->url, sizeof(cmd.filepath));
                strlcpy(cmd.source_id, source->id, sizeof(cmd.source_id));
            }
            else
            {
                const int index = music_player_get_current_index() >= 0 ? music_player_get_current_index() : 0;
                if (index < 0 || index >= total_tracks_found)
                {
                    if (error && error_size) strlcpy(error, "Không có bài hát trên thẻ SD", error_size);
                    xSemaphoreGive(music_ai_action_mutex);
                    return false;
                }
                cmd.type = MUSIC_CMD_PLAY_INDEX;
                cmd.track_idx = index;
                strlcpy(cmd.filepath, playlist[index].filepath, sizeof(cmd.filepath));
            }
            break;
        case AI_MUSIC_ACTION_PAUSE: cmd.type = MUSIC_CMD_PAUSE; break;
        case AI_MUSIC_ACTION_RESUME: cmd.type = MUSIC_CMD_RESUME; break;
        case AI_MUSIC_ACTION_STOP: cmd.type = MUSIC_CMD_STOP; break;
        case AI_MUSIC_ACTION_VOLUME: cmd.type = MUSIC_CMD_SET_VOLUME; cmd.param = action->volume; break;
        default: break;
    }
    bool ok = false;
    if (cmd.type != MUSIC_CMD_NONE && enqueue_music_command(cmd))
    {
        const TickType_t started = xTaskGetTickCount();
        const TickType_t timeout = pdMS_TO_TICKS(timeout_ms);
        MusicCommandAck ack = {};
        while (xQueueReceive(music_ack_queue, &ack, timeout) == pdTRUE)
        {
            if (ack.request_id == cmd.request_id) { ok = ack.ok; break; }
            if (xTaskGetTickCount() - started >= timeout) break;
        }
    }
    if (!ok && error && error_size && error[0] == '\0')
        strlcpy(error, "Thiết bị không thực hiện được lệnh nhạc", error_size);
    xSemaphoreGive(music_ai_action_mutex);
    return ok;
}
