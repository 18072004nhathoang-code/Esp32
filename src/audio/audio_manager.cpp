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
#include "firmware_contracts.h"
#include "service_state_logic.h"
#include "xiaozhi_session_logic.h"

// Quản lý trạng thái hệ thống âm thanh
static bool is_initialized = false;
static uint8_t master_volume = 80; // 0 - 100%
static bool pa_enabled = false;
static bool codec_ready = false;
static bool codec_muted = true;
static uint32_t codec_sample_rate = 0;
static uint16_t codec_mclk_multiple = 0;
static bool speaker_self_test_running = false;
static TaskHandle_t speaker_self_test_task_handle = nullptr;

// Bộ đệm ghi âm trong Octal PSRAM (8MB)
static int16_t *psram_record_buf = nullptr;
static uint32_t record_sample_capacity = AUDIO_MAX_SAMPLES;
static uint32_t recorded_samples_count = 0;
enum RecordingRunState : uint8_t { RECORD_IDLE, RECORD_STARTING, RECORD_ACTIVE, RECORD_SNAPSHOTTING };
static RecordingRunState recording_state = RECORD_IDLE;
static uint32_t recording_start_token = 0;
static uint32_t recording_next_token = 0;

struct RecordingSnapshot
{
    int16_t *samples;
    size_t count;
    uint32_t generation;
    uint32_t refs;
    bool current;
};
static RecordingSnapshot *current_recording = nullptr;
static uint32_t recording_generation = 0;

// Trạng thái phát lại
static bool playback_active = false;
static bool playback_starting = false;
static uint32_t playback_start_token = 0;
static uint32_t playback_next_token = 0;
static uint32_t playback_sample_idx = 0;
static uint32_t playback_owner_session = 0;
static AudioRecordingLease playback_lease = {};
static volatile AudioRecordingFileState recording_file_state = AUDIO_FILE_NONE;
static TaskHandle_t recording_export_task_handle = nullptr;
static constexpr const char *kRecordingPath = "/voice/last_recording.wav";
static constexpr const char *kRecordingTempPath = "/voice/last_recording.wav.tmp";
static constexpr const char *kRecordingBackupPath = "/voice/last_recording.wav.bak";

// Đo lường Microphone thời gian thực
static uint8_t current_mic_level = 0;     // 0 - 100%
static float current_mic_db = -60.0f;     // -60 to 0 dB
static int16_t waveform_history[128] = {0};
static size_t waveform_head = 0;

// FreeRTOS Task & Mutex
static TaskHandle_t audio_task_handle = nullptr;
static SemaphoreHandle_t audio_i2s_tx_mutex = nullptr; // Mutex độc quyền đường truyền TX i2s_write
static SemaphoreHandle_t audio_codec_mutex = nullptr;
// Protects recorder/playback state, the shared PSRAM buffer and waveform telemetry
// across the audio worker (core 0) and LVGL/application tasks (core 1).
static SemaphoreHandle_t audio_state_mutex = nullptr;

// Máy trạng thái phân quyền I2S phần cứng (Exclusive Ownership với RefCount Lease)
static volatile AudioOwner current_audio_owner = AUDIO_OWNER_NONE;
static volatile uint32_t audio_owner_refcount = 0;
static uint32_t audio_owner_session = 0;
static uint32_t audio_next_session = 0;
static SemaphoreHandle_t audio_owner_mutex = nullptr;
static AudioOwner audio_owner_transition = AUDIO_OWNER_NONE;
static bool i2s_duplex_installed = false;
static portMUX_TYPE audio_hw_mux = portMUX_INITIALIZER_UNLOCKED;

enum AudioAsyncCommandType : uint8_t
{
    AUDIO_ASYNC_SOUND_EFFECT = 1,
    AUDIO_ASYNC_START_RECORDING,
    AUDIO_ASYNC_START_PLAYBACK,
    AUDIO_ASYNC_SET_VOLUME
};

struct AudioAsyncCommand
{
    AudioAsyncCommandType type;
    uint8_t value;
    uint32_t generation;
};

static QueueHandle_t audio_command_queue = nullptr;
static uint32_t recording_command_generation = 0;
static uint32_t playback_command_generation = 0;
static portMUX_TYPE audio_command_mux = portMUX_INITIALIZER_UNLOCKED;
static uint32_t active_recording_command_generation = 0;
static uint32_t active_playback_command_generation = 0;
struct AudioCommandAck
{
    uint32_t generation;
    bool ok;
    bool snapshot_valid;
    uint32_t snapshot_generation;
    size_t sample_count;
};
static AudioCommandAck recording_acks[8] = {};
static uint32_t last_completed_recording_generation = 0;
static uint32_t last_completed_snapshot_generation = 0;
static size_t last_completed_sample_count = 0;

enum RecordingControlType : uint8_t
{
    RECORD_CONTROL_NONE = 0,
    RECORD_CONTROL_STOP,
    RECORD_CONTROL_CANCEL
};

struct AudioControlMailbox
{
    bool recording_pending;
    RecordingControlType recording_type;
    uint32_t recording_cancel_through;
    uint32_t recording_request_id;
    bool playback_stop_pending;
    uint32_t playback_cancel_through;
    uint32_t playback_request_id;
};
static AudioControlMailbox audio_control_mailbox = {};
struct RecordingControlRequest
{
    RecordingControlType type;
    uint32_t request_id;
    uint32_t cancel_through;
};
static RecordingControlRequest recording_control_queue[8] = {};
static uint8_t recording_control_head = 0;
static uint8_t recording_control_tail = 0;
static uint8_t recording_control_count = 0;

// The audio worker calls codec/I2C and tone code deeply. Keep DMA scratch out
// of its task stack so startup chimes cannot exhaust the FreeRTOS stack.
static constexpr size_t AUDIO_DMA_FRAME_COUNT = 256;
alignas(4) static uint8_t audio_dma_bytes[AUDIO_DMA_FRAME_COUNT * 2 * sizeof(int16_t)] = {};
alignas(4) static uint8_t audio_rx_bytes[sizeof(audio_dma_bytes) + 4] = {};
alignas(4) static int16_t audio_tx_frames[AUDIO_DMA_FRAME_COUNT * 2] = {};

static uint32_t next_command_generation(uint32_t &generation, uint32_t *previous = nullptr)
{
    portENTER_CRITICAL(&audio_command_mux);
    if (previous) *previous = generation;
    ++generation;
    if (generation == 0) ++generation;
    const uint32_t issued = generation;
    portEXIT_CRITICAL(&audio_command_mux);
    return issued;
}

static bool command_generation_current(uint32_t expected, uint32_t &generation)
{
    portENTER_CRITICAL(&audio_command_mux);
    const bool current = service_generation_current(expected, generation, false);
    portEXIT_CRITICAL(&audio_command_mux);
    return current;
}

static void acknowledge_recording_command(uint32_t generation, bool ok,
                                          bool snapshot_valid = false,
                                          uint32_t snapshot_generation = 0,
                                          size_t sample_count = 0)
{
    if (generation == 0) return;
    portENTER_CRITICAL(&audio_command_mux);
    AudioCommandAck &slot = recording_acks[generation %
                                            (sizeof(recording_acks) / sizeof(recording_acks[0]))];
    slot.ok = ok;
    slot.generation = generation;
    slot.snapshot_valid = snapshot_valid;
    slot.snapshot_generation = snapshot_generation;
    slot.sample_count = sample_count;
    portEXIT_CRITICAL(&audio_command_mux);
}

static bool post_recording_control(RecordingControlType type, uint32_t request_id,
                                   uint32_t cancel_through)
{
    bool accepted = false;
    portENTER_CRITICAL(&audio_command_mux);
    if (recording_control_count <
        sizeof(recording_control_queue) / sizeof(recording_control_queue[0]))
    {
        recording_control_queue[recording_control_tail] = { type, request_id, cancel_through };
        recording_control_tail = static_cast<uint8_t>(
            (recording_control_tail + 1U) %
            (sizeof(recording_control_queue) / sizeof(recording_control_queue[0])));
        ++recording_control_count;
        accepted = true;
    }
    portEXIT_CRITICAL(&audio_command_mux);
    if (accepted && audio_task_handle) xTaskNotifyGive(audio_task_handle);
    return accepted;
}

static void post_playback_stop(uint32_t request_id, uint32_t cancel_through)
{
    portENTER_CRITICAL(&audio_command_mux);
    audio_control_mailbox.playback_stop_pending = true;
    audio_control_mailbox.playback_cancel_through = cancel_through;
    audio_control_mailbox.playback_request_id = request_id;
    portEXIT_CRITICAL(&audio_command_mux);
    if (audio_task_handle) xTaskNotifyGive(audio_task_handle);
}

static AudioControlMailbox take_audio_controls()
{
    portENTER_CRITICAL(&audio_command_mux);
    AudioControlMailbox pending = {};
    if (recording_control_count > 0)
    {
        pending.recording_pending = true;
        const RecordingControlRequest &request = recording_control_queue[recording_control_head];
        pending.recording_type = request.type;
        pending.recording_request_id = request.request_id;
        pending.recording_cancel_through = request.cancel_through;
        recording_control_head = static_cast<uint8_t>(
            (recording_control_head + 1U) %
            (sizeof(recording_control_queue) / sizeof(recording_control_queue[0])));
        --recording_control_count;
    }
    else if (audio_control_mailbox.recording_pending)
    {
        pending.recording_pending = true;
        pending.recording_type = audio_control_mailbox.recording_type;
        pending.recording_request_id = audio_control_mailbox.recording_request_id;
        pending.recording_cancel_through = audio_control_mailbox.recording_cancel_through;
        audio_control_mailbox.recording_pending = false;
        audio_control_mailbox.recording_request_id = 0;
    }

    if (audio_control_mailbox.playback_stop_pending)
    {
        pending.playback_stop_pending = true;
        pending.playback_cancel_through = audio_control_mailbox.playback_cancel_through;
        pending.playback_request_id = audio_control_mailbox.playback_request_id;
        audio_control_mailbox.playback_stop_pending = false;
        audio_control_mailbox.playback_request_id = 0;
    }
    portEXIT_CRITICAL(&audio_command_mux);
    return pending;
}

static void post_recording_cancel_urgent(uint32_t request_id, uint32_t cancel_through)
{
    portENTER_CRITICAL(&audio_command_mux);
    if (recording_control_count <
        sizeof(recording_control_queue) / sizeof(recording_control_queue[0]))
    {
        recording_control_queue[recording_control_tail] = { RECORD_CONTROL_CANCEL, request_id, cancel_through };
        recording_control_tail = static_cast<uint8_t>(
            (recording_control_tail + 1U) %
            (sizeof(recording_control_queue) / sizeof(recording_control_queue[0])));
        ++recording_control_count;
    }
    else
    {
        if (audio_control_mailbox.recording_pending && audio_control_mailbox.recording_request_id != 0 &&
            audio_control_mailbox.recording_request_id != request_id)
        {
            acknowledge_recording_command(audio_control_mailbox.recording_request_id, false);
        }
        audio_control_mailbox.recording_pending = true;
        audio_control_mailbox.recording_type = RECORD_CONTROL_CANCEL;
        audio_control_mailbox.recording_cancel_through = cancel_through;
        audio_control_mailbox.recording_request_id = request_id;
    }
    portEXIT_CRITICAL(&audio_command_mux);
    if (audio_task_handle) xTaskNotifyGive(audio_task_handle);
}

static void rollback_command_generation(uint32_t &generation, uint32_t issued, uint32_t previous)
{
    portENTER_CRITICAL(&audio_command_mux);
    if (generation == issued) generation = previous;
    portEXIT_CRITICAL(&audio_command_mux);
}

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
static portMUX_TYPE audio_task_state_mux = portMUX_INITIALIZER_UNLOCKED;
static uint32_t audio_pause_request_id = 0;
static uint32_t audio_pause_ack_id = 0;

static void put_le16(uint8_t *p, uint16_t value)
{
    p[0] = value & 0xFF;
    p[1] = (value >> 8) & 0xFF;
}

static uint32_t get_le32(const uint8_t *p)
{
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

static uint16_t get_le16(const uint8_t *p)
{
    return static_cast<uint16_t>(p[0]) |
           static_cast<uint16_t>(static_cast<uint16_t>(p[1]) << 8);
}

static void stop_playback_sync(void);
static bool stop_recording_for_generation(uint32_t cancel_through, bool discard,
                                          bool *out_snapshot_valid = nullptr,
                                          uint32_t *out_snapshot_generation = nullptr,
                                          size_t *out_sample_count = nullptr);
static bool stop_playback_for_generation(uint32_t cancel_through);
static void play_sound_effect_sync(SoundEffect fx);
static bool start_recording_transaction(uint32_t max_duration_sec, uint32_t expected_generation);
static bool start_playback_transaction(uint32_t expected_generation);

static void set_pa_hardware_locked(bool enabled)
{
    pa_enabled = enabled;
    digitalWrite(AUDIO_PA_PIN, enabled ? LOW : HIGH);
}

static void put_le32(uint8_t *p, uint32_t value)
{
    p[0] = value & 0xFF;
    p[1] = (value >> 8) & 0xFF;
    p[2] = (value >> 16) & 0xFF;
    p[3] = (value >> 24) & 0xFF;
}

static void free_snapshot(RecordingSnapshot *snapshot)
{
    if (!snapshot) return;
    if (snapshot->samples) free(snapshot->samples);
    delete snapshot;
}

static bool publish_recording_snapshot(size_t count, uint32_t *created_generation = nullptr)
{
    if (created_generation) *created_generation = 0;
    RecordingSnapshot *fresh = nullptr;
    if (count > 0)
    {
        fresh = new RecordingSnapshot{};
        if (fresh)
        {
            fresh->samples = static_cast<int16_t *>(heap_caps_malloc(
                count * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
            if (!fresh->samples)
                fresh->samples = static_cast<int16_t *>(malloc(count * sizeof(int16_t)));
            if (!fresh->samples)
            {
                delete fresh;
                fresh = nullptr;
            }
        }
        if (fresh)
        {
            memcpy(fresh->samples, psram_record_buf, count * sizeof(int16_t));
            fresh->count = count;
            fresh->refs = 1; // Global current-recording reference.
            fresh->current = true;
        }
    }

    // Allocation failure must never invalidate the last known-good recording:
    // exporters and uploads may still hold immutable leases to it.
    if (!fresh)
    {
        if (audio_state_mutex && xSemaphoreTake(audio_state_mutex, portMAX_DELAY) == pdTRUE)
        {
            recording_state = RECORD_IDLE;
            active_recording_command_generation = 0;
            recording_file_state = (count > 0) ? AUDIO_FILE_ERROR : AUDIO_FILE_NONE;
            xSemaphoreGive(audio_state_mutex);
        }
        return false;
    }

    RecordingSnapshot *discard = nullptr;
    if (!audio_state_mutex || xSemaphoreTake(audio_state_mutex, portMAX_DELAY) != pdTRUE)
    {
        free_snapshot(fresh);
        return false;
    }
    RecordingSnapshot *old = current_recording;
    if (old)
    {
        old->current = false;
        if (old->refs > 0) --old->refs;
        if (old->refs == 0) discard = old;
    }
    current_recording = fresh;
    if (fresh)
    {
        fresh->generation = ++recording_generation;
        if (created_generation) *created_generation = fresh->generation;
    }
    recording_state = RECORD_IDLE;
    active_recording_command_generation = 0;
    recording_file_state = AUDIO_FILE_NONE;
    xSemaphoreGive(audio_state_mutex);
    free_snapshot(discard);
    return fresh != nullptr;
}

static bool valid_recording_file_locked(fs::FS &fs, const char *path)
{
    File file = fs.open(path, FILE_READ);
    if (!file || file.size() < 44)
    {
        if (file) file.close();
        return false;
    }
    uint8_t header[44] = {};
    const size_t read = file.read(header, sizeof(header));
    const size_t total_size = file.size();
    file.close();
    if (read != sizeof(header) || memcmp(header, "RIFF", 4) != 0 ||
        memcmp(header + 8, "WAVEfmt ", 8) != 0 || memcmp(header + 36, "data", 4) != 0)
        return false;
    const uint32_t data_size = get_le32(header + 40);
    return get_le32(header + 4) == data_size + 36U && get_le32(header + 16) == 16U &&
           get_le16(header + 20) == 1U && get_le16(header + 22) == 1U &&
           get_le32(header + 24) == AUDIO_SAMPLE_RATE &&
           get_le32(header + 28) == AUDIO_SAMPLE_RATE * sizeof(int16_t) &&
           get_le16(header + 32) == sizeof(int16_t) && get_le16(header + 34) == 16U &&
           (data_size % sizeof(int16_t)) == 0U &&
           total_size == static_cast<size_t>(data_size) + sizeof(header);
}

static bool recover_recording_files_locked(fs::FS &fs)
{
    const bool final_valid = fs.exists(kRecordingPath) &&
                             valid_recording_file_locked(fs, kRecordingPath);
    if (final_valid)
    {
        if (fs.exists(kRecordingTempPath)) fs.remove(kRecordingTempPath);
        if (fs.exists(kRecordingBackupPath)) fs.remove(kRecordingBackupPath);
        return true;
    }

    if (fs.exists(kRecordingPath) && !fs.remove(kRecordingPath)) return false;
    const bool backup_exists = fs.exists(kRecordingBackupPath);
    const bool backup_valid = backup_exists &&
                              valid_recording_file_locked(fs, kRecordingBackupPath);
    const bool backup_restored = backup_valid &&
                                 fs.rename(kRecordingBackupPath, kRecordingPath);
    if (backup_restored)
    {
        if (fs.exists(kRecordingTempPath)) fs.remove(kRecordingTempPath);
        Serial.println("[AUDIO][WAV] Recovered last recording from .bak");
        return true;
    }
    // A valid backup that could not be renamed is the last known-good file.
    // Preserve it (and any valid temp) so a later boot can retry recovery.
    if (transactional_keep_recovery_file(backup_valid, backup_restored)) return false;
    if (backup_exists) fs.remove(kRecordingBackupPath);

    const bool temp_exists = fs.exists(kRecordingTempPath);
    const bool temp_valid = temp_exists && valid_recording_file_locked(fs, kRecordingTempPath);
    const bool temp_restored = temp_valid && fs.rename(kRecordingTempPath, kRecordingPath);
    if (temp_restored)
    {
        Serial.println("[AUDIO][WAV] Recovered completed recording from .tmp");
        return true;
    }
    if (temp_exists && !temp_valid) fs.remove(kRecordingTempPath);
    if (transactional_keep_recovery_file(temp_valid, temp_restored))
        Serial.println("[AUDIO][WAV] Valid .tmp retained for recovery retry");
    return false;
}

static void recording_export_task(void *arg)
{
    AudioRecordingLease *lease = static_cast<AudioRecordingLease *>(arg);
    bool ok = false;
    size_t total_written = 0;
    bool backup_created = false;
    bool new_file_installed = false;
    bool commit_verified = false;
    const size_t expected_file_size = lease
        ? 44U + lease->sample_count * sizeof(int16_t) : 0U;
    if (lease && lease->samples && lease->sample_count > 0 &&
        storage_is_available() && storage_lock(1000))
    {
        fs::FS &fs = storage_get_fs();
        bool directory_ready = fs.exists("/voice") || fs.mkdir("/voice");
        if (directory_ready)
        {
            (void)recover_recording_files_locked(fs);
            const bool final_valid = fs.exists(kRecordingPath) &&
                                     valid_recording_file_locked(fs, kRecordingPath);
            const bool unresolved_backup = fs.exists(kRecordingBackupPath) &&
                valid_recording_file_locked(fs, kRecordingBackupPath);
            const bool unresolved_temp = fs.exists(kRecordingTempPath) &&
                valid_recording_file_locked(fs, kRecordingTempPath);
            if (!final_valid && (unresolved_backup || unresolved_temp))
                directory_ready = false;
        }
        if (directory_ready && fs.exists(kRecordingTempPath)) fs.remove(kRecordingTempPath);
        File file = directory_ready ? fs.open(kRecordingTempPath, FILE_WRITE) : File();
        if (file)
        {
            const uint32_t data_bytes = lease->sample_count * sizeof(int16_t);
            uint8_t header[44] = {};
            memcpy(header, "RIFF", 4); put_le32(header + 4, 36 + data_bytes);
            memcpy(header + 8, "WAVEfmt ", 8); put_le32(header + 16, 16);
            put_le16(header + 20, 1); put_le16(header + 22, 1);
            put_le32(header + 24, AUDIO_SAMPLE_RATE);
            put_le32(header + 28, AUDIO_SAMPLE_RATE * sizeof(int16_t));
            put_le16(header + 32, sizeof(int16_t)); put_le16(header + 34, 16);
            memcpy(header + 36, "data", 4); put_le32(header + 40, data_bytes);
            const size_t header_written = file.write(header, sizeof(header));
            total_written += header_written;
            ok = header_written == sizeof(header);
            const uint8_t *raw = reinterpret_cast<const uint8_t *>(lease->samples);
            size_t remaining = data_bytes;
            while (ok && remaining > 0)
            {
                size_t chunk = remaining > 4096 ? 4096 : remaining;
                const size_t written = file.write(raw, chunk);
                total_written += written;
                ok = written == chunk;
                raw += chunk;
                remaining -= chunk;
                vTaskDelay(1);
            }
            file.flush();
            file.close();
            ok = ok && valid_recording_file_locked(fs, kRecordingTempPath);
        }

        if (ok)
        {
            const bool had_old = fs.exists(kRecordingPath);
            // A valid final is authoritative, so only then may a stale backup
            // be removed before creating this transaction's own backup.
            if (had_old && fs.exists(kRecordingBackupPath) &&
                valid_recording_file_locked(fs, kRecordingPath))
                fs.remove(kRecordingBackupPath);

            const bool backup_ready = !had_old ||
                (backup_created = fs.rename(kRecordingPath, kRecordingBackupPath));
            if (transactional_replace_can_commit(expected_file_size, total_written,
                                                  true, backup_ready))
            {
                new_file_installed = fs.rename(kRecordingTempPath, kRecordingPath);
                commit_verified = new_file_installed &&
                                  valid_recording_file_locked(fs, kRecordingPath);
            }
            ok = commit_verified;
            if (commit_verified)
            {
                if (backup_created && fs.exists(kRecordingBackupPath))
                    fs.remove(kRecordingBackupPath);
            }
            else if (backup_created)
            {
                // Never remove an untouched old final after backup rename
                // failed.  Remove final only when this transaction installed it.
                if (transactional_remove_new_final(new_file_installed, commit_verified) &&
                    fs.exists(kRecordingPath))
                    fs.remove(kRecordingPath);
                // On restore failure keep .bak for the next recovery attempt.
                if (!fs.exists(kRecordingPath) && fs.exists(kRecordingBackupPath))
                    (void)fs.rename(kRecordingBackupPath, kRecordingPath);
            }
        }
        storage_unlock();
    }
    if (lease)
    {
        audio_release_recording_lease(lease);
        delete lease;
    }
    if (audio_state_mutex && xSemaphoreTake(audio_state_mutex, portMAX_DELAY) == pdTRUE)
    {
        recording_file_state = ok ? AUDIO_FILE_SAVED : AUDIO_FILE_ERROR;
        recording_export_task_handle = nullptr;
        xSemaphoreGive(audio_state_mutex);
    }
    vTaskDelete(nullptr);
}

static void schedule_recording_export(void)
{
    if (!audio_state_mutex) return;
    AudioRecordingLease *lease = new AudioRecordingLease{};
    if (!lease || !audio_acquire_recording_lease(lease))
    {
        delete lease;
        return;
    }
    if (xSemaphoreTake(audio_state_mutex, pdMS_TO_TICKS(100)) != pdTRUE)
    {
        audio_release_recording_lease(lease);
        delete lease;
        return;
    }
    if (recording_export_task_handle)
    {
        xSemaphoreGive(audio_state_mutex);
        audio_release_recording_lease(lease);
        delete lease;
        return;
    }
    if (!storage_is_available())
    {
        recording_file_state = AUDIO_FILE_ERROR;
        xSemaphoreGive(audio_state_mutex);
        audio_release_recording_lease(lease);
        delete lease;
        return;
    }
    recording_file_state = AUDIO_FILE_SAVING;
    BaseType_t created = xTaskCreatePinnedToCore(recording_export_task, "VoiceWavSave", 4096,
                                                 lease, 1, &recording_export_task_handle, 0);
    if (created != pdPASS)
    {
        recording_export_task_handle = nullptr;
        recording_file_state = AUDIO_FILE_ERROR;
    }
    xSemaphoreGive(audio_state_mutex);
    if (created != pdPASS)
    {
        audio_release_recording_lease(lease);
        delete lease;
    }
}

/* Cấu hình và cài đặt Driver I2S Duplex (16kHz 16-bit Duplex) cho Microphone & Tone/Voice */
bool audio_install_duplex_driver(void)
{
    if (audio_is_driver_installed()) return true;

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
        .fixed_mclk = 0,
        .mclk_multiple = I2S_MCLK_MULTIPLE_256,
        .bits_per_chan = I2S_BITS_PER_CHAN_16BIT
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

    portENTER_CRITICAL(&audio_hw_mux);
    i2s_duplex_installed = true;
    portEXIT_CRITICAL(&audio_hw_mux);
    Serial.printf("[AUDIO][I2S] READY Fs=%u PCM16 stereo MCLK=%u BCLK=%d WS=%d DOUT=%d DIN=%d\n",
                  AUDIO_SAMPLE_RATE, AUDIO_SAMPLE_RATE * 256U, AUDIO_I2S_BCLK, AUDIO_I2S_WS,
                  AUDIO_I2S_DOUT, AUDIO_I2S_DIN);
    return true;
}

bool audio_drain_tx(uint32_t timeout_ms)
{
    if (timeout_ms == 0) return false;
    // Arduino-ESP32's legacy I2S API has no wait_tx_done. Queue a silence
    // marker behind all existing PCM, then allow the bounded DMA depth to run.
    int16_t silence[32 * 2] = {};
    size_t written = 0;
    const esp_err_t err = i2s_write(I2S_NUM_0, silence, sizeof(silence), &written,
                                    pdMS_TO_TICKS(timeout_ms));
    if (err != ESP_OK || written != sizeof(silence)) return false;
    vTaskDelay(pdMS_TO_TICKS(min<uint32_t>(timeout_ms, 200U)));
    return true;
}

bool audio_uninstall_duplex_driver(void)
{
    if (!audio_is_driver_installed()) return true;

    i2s_zero_dma_buffer(I2S_NUM_0);
    esp_err_t err = i2s_driver_uninstall(I2S_NUM_0);
    if (err == ESP_OK)
    {
        portENTER_CRITICAL(&audio_hw_mux);
        i2s_duplex_installed = false;
        portEXIT_CRITICAL(&audio_hw_mux);
        Serial.println("[AUDIO] 🔌 Đã gỡ bỏ I2S Duplex Driver để nhường cổng I2S_NUM_0.");
        return true;
    }
    else
    {
        Serial.printf("[AUDIO] Cảnh báo khi gỡ I2S Driver: 0x%X\n", err);
        return false;
    }
}

bool audio_is_driver_installed(void)
{
    portENTER_CRITICAL(&audio_hw_mux);
    const bool installed = i2s_duplex_installed;
    portEXIT_CRITICAL(&audio_hw_mux);
    return installed;
}

bool audio_manager_pause_task_sync(uint32_t timeout_ms)
{
    if (audio_task_handle == nullptr) return true;

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
    portENTER_CRITICAL(&audio_task_state_mux);
    if (audio_task_state == AUDIO_TASK_PAUSED)
    {
        portEXIT_CRITICAL(&audio_task_state_mux);
        return true;
    }
    ++audio_pause_request_id;
    if (audio_pause_request_id == 0) ++audio_pause_request_id;
    const uint32_t request_id = audio_pause_request_id;
    audio_task_state = AUDIO_TASK_PAUSE_REQUESTED;
    portEXIT_CRITICAL(&audio_task_state_mux);
    xTaskNotifyGive(audio_task_handle);

    // Đợi Audio Task gửi ACK xác nhận đã ra khỏi mọi hàm I2S DMA
    if (xSemaphoreTake(audio_task_ack_sem, pdMS_TO_TICKS(timeout_ms)) == pdTRUE)
    {
        portENTER_CRITICAL(&audio_task_state_mux);
        const bool current_ack = audio_task_state == AUDIO_TASK_PAUSED &&
                                 audio_pause_ack_id == request_id;
        portEXIT_CRITICAL(&audio_task_state_mux);
        if (current_ack)
        {
            Serial.println("[AUDIO] 🛑 Audio Task đã dừng an toàn và gửi ACK.");
            return true;
        }
    }
    Serial.println("[AUDIO] ⚠️ Timeout chờ ACK dừng Audio Task!");
    // Revoke this request. A late worker ACK must not strand Audio_Task paused.
    portENTER_CRITICAL(&audio_task_state_mux);
    if (audio_pause_request_id == request_id &&
        (audio_task_state == AUDIO_TASK_PAUSE_REQUESTED ||
         audio_task_state == AUDIO_TASK_PAUSED))
        audio_task_state = AUDIO_TASK_RESUME_REQUESTED;
    portEXIT_CRITICAL(&audio_task_state_mux);
    xTaskNotifyGive(audio_task_handle);
    return false;
}

void audio_manager_resume_task(void)
{
    portENTER_CRITICAL(&audio_task_state_mux);
    const bool resume = audio_task_state == AUDIO_TASK_PAUSED ||
                        audio_task_state == AUDIO_TASK_PAUSE_REQUESTED;
    if (resume)
    {
        audio_task_state = AUDIO_TASK_RESUME_REQUESTED;
    }
    portEXIT_CRITICAL(&audio_task_state_mux);
    if (resume && audio_task_handle) xTaskNotifyGive(audio_task_handle);
    if (resume) Serial.println("[AUDIO] ▶ Đã gửi yêu cầu khôi phục hoạt động cho Audio Task.");
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

    // Re-entrant owner requests are real leases and must be balanced. Call-site
    // guards prevent Play/Resume from acquiring twice for one logical session.
    if (audio_owner_transition != AUDIO_OWNER_NONE)
    {
        xSemaphoreGive(audio_owner_mutex);
        return false;
    }
    if (current_audio_owner == requester)
    {
        ++audio_owner_refcount;
        Serial.printf("[AUDIO][OWNER] owner=%u session=%u ref=%u (shared subsystem lease)\n",
                      requester, audio_owner_session, audio_owner_refcount);
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
        // Reserve the transition, but never hold the owner mutex while waiting
        // for an ACK from Audio_Task (the worker also queries ownership).
        audio_owner_transition = AUDIO_OWNER_MUSIC;
        xSemaphoreGive(audio_owner_mutex);

        const bool pause_acked = audio_manager_pause_task_sync(300);
        if (!pause_acked)
        {
            Serial.println("[AUDIO] ❌ Lỗi: Không thể pause Audio Task kịp thời để nhường I2S cho MUSIC!");
            if (xSemaphoreTake(audio_owner_mutex, portMAX_DELAY) == pdTRUE)
            {
                if (audio_owner_transition == AUDIO_OWNER_MUSIC)
                    audio_owner_transition = AUDIO_OWNER_NONE;
                xSemaphoreGive(audio_owner_mutex);
            }
            return false;
        }

        const bool uninstall_ok = audio_uninstall_duplex_driver();
        if (!audio_music_handoff_can_grant(pause_acked, uninstall_ok))
        {
            audio_manager_resume_task();
            if (xSemaphoreTake(audio_owner_mutex, portMAX_DELAY) == pdTRUE)
            {
                if (audio_owner_transition == AUDIO_OWNER_MUSIC)
                    audio_owner_transition = AUDIO_OWNER_NONE;
                xSemaphoreGive(audio_owner_mutex);
            }
            return false;
        }

        if (xSemaphoreTake(audio_owner_mutex, portMAX_DELAY) != pdTRUE)
        {
            const bool driver_ok = audio_install_duplex_driver();
            const bool codec_ok = driver_ok &&
                audio_codec_configure_for_stream(AUDIO_SAMPLE_RATE, 256);
            if (audio_duplex_restore_ready(driver_ok, codec_ok)) audio_manager_resume_task();
            else Serial.println("[AUDIO] ❌ MUSIC handoff rollback could not restore duplex/codec");
            return false;
        }
        if (audio_owner_transition != AUDIO_OWNER_MUSIC ||
            current_audio_owner != AUDIO_OWNER_NONE)
        {
            audio_owner_transition = AUDIO_OWNER_NONE;
            xSemaphoreGive(audio_owner_mutex);
            const bool driver_ok = audio_install_duplex_driver();
            const bool codec_ok = driver_ok &&
                audio_codec_configure_for_stream(AUDIO_SAMPLE_RATE, 256);
            if (audio_duplex_restore_ready(driver_ok, codec_ok)) audio_manager_resume_task();
            else Serial.println("[AUDIO] ❌ MUSIC handoff rollback could not restore duplex/codec");
            return false;
        }
        current_audio_owner = AUDIO_OWNER_MUSIC;
        audio_owner_refcount = 1;
        audio_owner_session = ++audio_next_session;
        if (audio_owner_session == 0) audio_owner_session = ++audio_next_session;
        audio_owner_transition = AUDIO_OWNER_NONE;
        Serial.printf("[AUDIO][OWNER] acquire MUSIC session=%u ref=1\n", audio_owner_session);
        xSemaphoreGive(audio_owner_mutex);
        return true;
    }

    // 2. Đối với các requester khác (SYSTEM, RECORDER, AI_VOICE):
    // Đảm bảo I2S Duplex Driver của AudioManager đã sẵn sàng
    if (!audio_is_driver_installed())
    {
        if (!audio_install_duplex_driver())
        {
            xSemaphoreGive(audio_owner_mutex);
            return false;
        }
    }

    current_audio_owner = requester;
    audio_owner_refcount = 1;
    audio_owner_session = ++audio_next_session;
    if (audio_owner_session == 0) audio_owner_session = ++audio_next_session;
    Serial.printf("[AUDIO][OWNER] acquire owner=%u session=%u ref=1\n",
                  requester, audio_owner_session);
    xSemaphoreGive(audio_owner_mutex);
    return true;
}

static bool release_ownership(AudioOwner requester, uint32_t expected_session, bool require_session)
{
    if (!audio_owner_mutex || xSemaphoreTake(audio_owner_mutex, portMAX_DELAY) != pdTRUE)
        return false;

    bool released = false;
    const bool owner_matches = current_audio_owner == requester;
    const bool session_matches = audio_session_cleanup_current(
        static_cast<uint8_t>(current_audio_owner), static_cast<uint8_t>(requester),
        audio_owner_session, expected_session);
    if (owner_matches && (!require_session || session_matches))
    {
        if (audio_owner_refcount > 1)
        {
            audio_owner_refcount--;
            released = true;
            Serial.printf("[AUDIO][OWNER] release owner=%u session=%u ref=%u\n",
                          requester, audio_owner_session, audio_owner_refcount);
        }
        else
        {
            const uint32_t completed_session = audio_owner_session;
            if (audio_codec_mutex && xSemaphoreTake(audio_codec_mutex, portMAX_DELAY) == pdTRUE)
            {
                set_pa_hardware_locked(false);
                xSemaphoreGive(audio_codec_mutex);
            }

            // Restore the duplex path before publishing OWNER_NONE so another
            // caller cannot acquire a half-restored I2S/codec configuration.
            bool restore_ok = true;
            if (requester == AUDIO_OWNER_MUSIC)
            {
                const bool driver_ok = audio_install_duplex_driver();
                bool codec_ok = false;
                if (driver_ok)
                {
                    codec_ok = audio_codec_configure_for_stream(AUDIO_SAMPLE_RATE, 256);
                    if (!codec_ok)
                        Serial.println("[AUDIO] ❌ I2S duplex restored but codec clock restore failed");
                }
                else
                {
                    Serial.println("[AUDIO] ❌ MUSIC released but duplex restore failed");
                }
                restore_ok = audio_duplex_restore_ready(driver_ok, codec_ok);
                if (restore_ok) audio_manager_resume_task();
            }
            if (!restore_ok)
            {
                // Keep MUSIC ownership published as an error/retry state. A
                // later release attempt retries restore instead of reporting
                // OWNER_NONE while duplex is unavailable.
                xSemaphoreGive(audio_owner_mutex);
                return false;
            }
            audio_owner_refcount = 0;
            current_audio_owner = AUDIO_OWNER_NONE;
            audio_owner_session = 0;
            released = true;
            Serial.printf("[AUDIO][OWNER] release owner=%u session=%u complete\n",
                          requester, completed_session);
        }
    }
    xSemaphoreGive(audio_owner_mutex);
    return released;
}

void audio_release_ownership(AudioOwner requester)
{
    (void)release_ownership(requester, 0, false);
}

bool audio_release_ownership_session(AudioOwner requester, uint32_t session_id)
{
    return release_ownership(requester, session_id, true);
}

uint32_t audio_get_owner_session(AudioOwner requester)
{
    if (!audio_owner_mutex || xSemaphoreTake(audio_owner_mutex, pdMS_TO_TICKS(20)) != pdTRUE)
        return 0;
    const uint32_t session = current_audio_owner == requester ? audio_owner_session : 0;
    xSemaphoreGive(audio_owner_mutex);
    return session;
}

bool audio_set_pa_for_session(AudioOwner requester, uint32_t session_id, bool enabled)
{
    if (!audio_owner_mutex || !audio_codec_mutex || session_id == 0 ||
        xSemaphoreTake(audio_owner_mutex, pdMS_TO_TICKS(100)) != pdTRUE)
        return false;
    const bool current = current_audio_owner == requester && audio_owner_session == session_id;
    bool applied = false;
    if (current && xSemaphoreTake(audio_codec_mutex, pdMS_TO_TICKS(100)) == pdTRUE)
    {
        set_pa_hardware_locked(enabled && master_volume > 0 && !codec_muted);
        applied = true;
        xSemaphoreGive(audio_codec_mutex);
    }
    xSemaphoreGive(audio_owner_mutex);
    return applied;
}

AudioOwner audio_get_current_owner(void)
{
    if (!audio_owner_mutex || xSemaphoreTake(audio_owner_mutex, pdMS_TO_TICKS(20)) != pdTRUE)
        return AUDIO_OWNER_NONE;
    const AudioOwner owner = current_audio_owner;
    xSemaphoreGive(audio_owner_mutex);
    return owner;
}

/* =========================================================================
 * CẤU HÌNH VÀ GHI DỮ LIỆU I2C CODEC ES8311 (NẾU CÓ)
 * ========================================================================= */
static bool es8311_write_reg(uint8_t reg, uint8_t val)
{
    return shared_i2c_write_reg(AUDIO_ES8311_ADDR, reg, val);
}

static bool es8311_read_reg(uint8_t reg, uint8_t &val)
{
    return shared_i2c_read_reg(AUDIO_ES8311_ADDR, reg, &val, 1);
}

static bool es8311_write_checked(uint8_t reg, uint8_t val, uint8_t verify_mask = 0xFF)
{
    uint8_t actual = 0;
    if (!es8311_write_reg(reg, val))
    {
        Serial.printf("[AUDIO][CODEC] write reg 0x%02X failed\n", reg);
        return false;
    }
    if (!es8311_read_reg(reg, actual) || (actual & verify_mask) != (val & verify_mask))
    {
        Serial.printf("[AUDIO][CODEC] readback reg 0x%02X expected=0x%02X actual=0x%02X\n",
                      reg, val, actual);
        return false;
    }
    return true;
}

static bool es8311_configure_clock(uint32_t sample_rate, uint16_t mclk_multiple)
{
    if (!codec_ready || sample_rate < 8000 || sample_rate > 96000 ||
        (mclk_multiple != 128 && mclk_multiple != 256)) return false;
    if (codec_sample_rate == sample_rate && codec_mclk_multiple == mclk_multiple) return true;

    // Coefficients are the ES8311 slave-mode 128Fs/256Fs entries used by
    // Espressif's codec driver. The ESP32 owns MCLK/BCLK/LRCK.
    const uint8_t pre_multiplier_bits = mclk_multiple == 128 ? 0x08 : 0x00; // x2 / x1
    const uint8_t dac_osr = sample_rate <= 16000 ? 0x20 : 0x10;
    bool ok = true;
    ok = es8311_write_checked(0x02, pre_multiplier_bits) && ok;
    ok = es8311_write_checked(0x05, 0x00) && ok; // ADC/DAC clock divide by 1
    ok = es8311_write_checked(0x03, 0x10) && ok; // single speed, ADC OSR 64Fs
    ok = es8311_write_checked(0x04, dac_osr) && ok;
    ok = es8311_write_checked(0x07, 0x00) && ok;
    ok = es8311_write_checked(0x08, 0xFF) && ok; // LRCK divider 256 in slave profile
    ok = es8311_write_checked(0x06, 0x03) && ok; // normal BCLK, divider coefficient 4
    if (!ok) return false;
    codec_sample_rate = sample_rate;
    codec_mclk_multiple = mclk_multiple;
    Serial.printf("[AUDIO][CODEC] clock Fs=%lu MCLK=%lu (%uFs) I2S/PCM16 slave\n",
                  static_cast<unsigned long>(sample_rate),
                  static_cast<unsigned long>(sample_rate * mclk_multiple), mclk_multiple);
    return true;
}

static bool es8311_set_muted(bool muted)
{
    if (!codec_ready) return false;
    uint8_t reg31 = 0;
    if (!es8311_read_reg(0x31, reg31)) return false;
    reg31 &= 0x9F;
    if (muted) reg31 |= 0x60; // DAC DSM + DEM mute
    if (!es8311_write_checked(0x31, reg31)) return false;
    codec_muted = muted;
    Serial.printf("[AUDIO][CODEC] mute=%s reg31=0x%02X\n", muted ? "ON" : "OFF", reg31);
    return true;
}

static bool es8311_init_codec(void)
{
    if (!shared_i2c_codec_is_detected())
    {
        Serial.println("[AUDIO] Không phát hiện ES8311; đường loa không khả dụng.");
        return false;
    }

    uint8_t chip_id1 = 0, chip_id2 = 0;
    if (!es8311_read_reg(0xFD, chip_id1) || !es8311_read_reg(0xFE, chip_id2) ||
        chip_id1 != 0x83 || chip_id2 != 0x11)
    {
        Serial.printf("[AUDIO][CODEC] ES8311 ID invalid: %02X %02X\n", chip_id1, chip_id2);
        return false;
    }

    Serial.println("[AUDIO][CODEC] ES8311 ID 83:11; applying verified init");
    bool ok = true;
    ok = es8311_write_checked(0x44, 0x08) && ok; // I2C noise immunity
    ok = es8311_write_checked(0x44, 0x08) && ok;
    ok = es8311_write_checked(0x00, 0x1F) && ok; // reset digital blocks
    delay(20);
    ok = es8311_write_checked(0x00, 0x00) && ok;
    ok = es8311_write_checked(0x00, 0x80) && ok; // CSM on, codec slave
    ok = es8311_write_checked(0x01, 0x3F) && ok; // external MCLK, all clocks enabled
    ok = es8311_write_checked(0x09, 0x0C) && ok; // DAC input: I2S, 16-bit, unmuted
    ok = es8311_write_checked(0x0A, 0x0C) && ok; // ADC output: I2S, 16-bit, unmuted
    ok = es8311_write_checked(0x0B, 0x00) && ok;
    ok = es8311_write_checked(0x0C, 0x00) && ok;
    ok = es8311_write_checked(0x10, 0x1F) && ok;
    ok = es8311_write_checked(0x11, 0x7F) && ok;
    ok = es8311_write_checked(0x13, 0x10) && ok; // differential output path
    ok = es8311_write_checked(0x1B, 0x0A) && ok;
    ok = es8311_write_checked(0x1C, 0x6A) && ok;
    ok = es8311_write_checked(0x44, 0x58) && ok; // internal ADC/DAC reference route
    if (!ok) return false;

    codec_ready = true;
    if (!es8311_configure_clock(AUDIO_SAMPLE_RATE, 256)) return false;
    ok = es8311_write_checked(0x17, 0xBF) && ok; // ADC unity gain
    ok = es8311_write_checked(0x0E, 0x02) && ok; // PGA/modulator powered
    ok = es8311_write_checked(0x12, 0x00) && ok; // DAC powered
    ok = es8311_write_checked(0x14, 0x1A) && ok; // analog mic PGA route
    ok = es8311_write_checked(0x0D, 0x01) && ok; // analog/reference powered
    ok = es8311_write_checked(0x15, 0x40) && ok;
    ok = es8311_write_checked(0x37, 0x08) && ok; // DACEQ bypass, no soft ramp
    ok = es8311_write_checked(0x45, 0x00) && ok;
    ok = es8311_write_checked(0x32, es8311_volume_register(master_volume)) && ok;
    ok = es8311_set_muted(false) && ok;
    if (!ok)
    {
        codec_ready = false;
        return false;
    }
    Serial.printf("[AUDIO][CODEC] READY volume=%u%% reg32=0x%02X PA=OFF\n",
                  master_volume, es8311_volume_register(master_volume));
    return true;
}

/* =========================================================================
 * BẬT/TẮT POWER AMPLIFIER (FM8002E / NS4168)
 * ========================================================================= */
void audio_set_pa_enabled(bool enabled)
{
    if (!audio_codec_mutex || xSemaphoreTake(audio_codec_mutex, pdMS_TO_TICKS(100)) != pdTRUE)
        return;
    set_pa_hardware_locked(enabled);
    xSemaphoreGive(audio_codec_mutex);
}

bool audio_is_pa_enabled(void)
{
    if (!audio_codec_mutex || xSemaphoreTake(audio_codec_mutex, pdMS_TO_TICKS(20)) != pdTRUE)
        return false;
    const bool enabled = pa_enabled;
    xSemaphoreGive(audio_codec_mutex);
    return enabled;
}

void audio_set_volume(uint8_t volume_percent)
{
    if (volume_percent > 100) volume_percent = 100;
    if (!audio_codec_mutex || xSemaphoreTake(audio_codec_mutex, pdMS_TO_TICKS(100)) != pdTRUE)
        return;
    master_volume = volume_percent;
    if (!codec_ready)
    {
        xSemaphoreGive(audio_codec_mutex);
        return;
    }
    const uint8_t reg_vol = es8311_volume_register(volume_percent);
    bool ok = es8311_write_checked(0x32, reg_vol);
    ok = es8311_set_muted(volume_percent == 0) && ok;
    if (volume_percent == 0 || !ok) set_pa_hardware_locked(false);
    Serial.printf("[AUDIO][CODEC] volume=%u%% reg32=0x%02X mute=%s PA=%s status=%s\n",
                  volume_percent, reg_vol, codec_muted ? "ON" : "OFF",
                  pa_enabled ? "ON" : "OFF", ok ? "OK" : "ERROR");
    xSemaphoreGive(audio_codec_mutex);
}

bool audio_set_volume_async(uint8_t volume_percent)
{
    if (!audio_command_queue) return false;
    const AudioAsyncCommand cmd = {
        AUDIO_ASYNC_SET_VOLUME,
        static_cast<uint8_t>(volume_percent > 100 ? 100 : volume_percent),
        0
    };
    return xQueueSend(audio_command_queue, &cmd, 0) == pdTRUE;
}

uint8_t audio_get_volume(void)
{
    if (!audio_codec_mutex || xSemaphoreTake(audio_codec_mutex, pdMS_TO_TICKS(20)) != pdTRUE)
        return 0;
    const uint8_t volume = master_volume;
    xSemaphoreGive(audio_codec_mutex);
    return volume;
}

bool audio_codec_configure_for_stream(uint32_t sample_rate, uint16_t mclk_multiple)
{
    if (!audio_codec_mutex || xSemaphoreTake(audio_codec_mutex, pdMS_TO_TICKS(200)) != pdTRUE)
        return false;
    const bool ok = es8311_configure_clock(sample_rate, mclk_multiple) &&
                    es8311_write_checked(0x32, es8311_volume_register(master_volume)) &&
                    es8311_set_muted(master_volume == 0);
    xSemaphoreGive(audio_codec_mutex);
    return ok;
}

static bool write_stereo_frames(const int16_t *frames, size_t frame_count, uint32_t timeout_ms)
{
    if (!frames || frame_count == 0 || !audio_i2s_tx_mutex ||
        xSemaphoreTake(audio_i2s_tx_mutex, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) return false;
    const uint8_t *cursor = reinterpret_cast<const uint8_t *>(frames);
    const size_t total_bytes = frame_count * 2U * sizeof(int16_t);
    size_t sent = 0;
    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    while (sent < total_bytes)
    {
        size_t written = 0;
        TickType_t now = xTaskGetTickCount();
        TickType_t remaining_ticks = deadline > now ? deadline - now : 0;
        if (remaining_ticks == 0) break;
        const esp_err_t err = i2s_write(I2S_NUM_0, cursor + sent, total_bytes - sent,
                                        &written, remaining_ticks);
        if (err != ESP_OK || written == 0 || written > total_bytes - sent)
        {
            Serial.printf("[AUDIO][I2S] TX error=0x%X requested=%u written=%u\n",
                          err, static_cast<unsigned>(total_bytes), static_cast<unsigned>(sent));
            break;
        }
        sent += written;
    }
    xSemaphoreGive(audio_i2s_tx_mutex);
    // A partial terminal write is a hard failure. Callers stop the stream instead
    // of retrying the partially accepted frame and duplicating audio.
    const bool complete = audio_write_completed(sent, total_bytes);
    if (!complete)
        Serial.printf("[AUDIO][I2S] TX partial requested=%u written=%u\n",
                      static_cast<unsigned>(total_bytes), static_cast<unsigned>(sent));
    return complete;
}

/* =========================================================================
 * FREERTOS AUDIO TASK CHẠY TRÊN CORE 0 (I2S DMA SAMPLING & RECORD/PLAYBACK)
 * ========================================================================= */
static void audio_background_task(void *pvParameters)
{
    size_t rx_carry_bytes = 0;
    size_t bytes_read = 0;
    uint32_t last_stack_report_ms = 0;

    while (1)
    {
        const uint32_t now_ms = millis();
        if (now_ms - last_stack_report_ms >= 30000U)
        {
            last_stack_report_ms = now_ms;
            Serial.printf("[AUDIO][STACK] Audio_Task high-water=%u bytes\n",
                          static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr) *
                                                sizeof(StackType_t)));
        }
        AudioControlMailbox controls;
        while ((controls = take_audio_controls()).recording_pending || controls.playback_stop_pending)
        {
            if (controls.recording_pending)
            {
                bool snap_valid = false;
                uint32_t snap_gen = 0;
                size_t snap_samples = 0;
                const bool applied = stop_recording_for_generation(
                    controls.recording_cancel_through,
                    controls.recording_type == RECORD_CONTROL_CANCEL,
                    &snap_valid, &snap_gen, &snap_samples);
                acknowledge_recording_command(controls.recording_request_id, applied,
                                              snap_valid, snap_gen, snap_samples);
            }
            if (controls.playback_stop_pending)
                (void)stop_playback_for_generation(controls.playback_cancel_through);
        }

        // 0a. Máy trạng thái Handshake dừng/khôi phục Audio Task an toàn
        AudioTaskState task_state;
        uint32_t pause_request = 0;
        portENTER_CRITICAL(&audio_task_state_mux);
        task_state = audio_task_state;
        pause_request = audio_pause_request_id;
        portEXIT_CRITICAL(&audio_task_state_mux);
        if (task_state == AUDIO_TASK_PAUSE_REQUESTED)
        {
            AudioAsyncCommand cancelled = {};
            while (audio_command_queue &&
                   xQueueReceive(audio_command_queue, &cancelled, 0) == pdTRUE)
            {
                if (cancelled.type == AUDIO_ASYNC_SET_VOLUME)
                    audio_set_volume(cancelled.value);
                else
                {
                    if (cancelled.type == AUDIO_ASYNC_START_RECORDING)
                        acknowledge_recording_command(cancelled.generation, false);
                    Serial.printf("[AUDIO] command %u cancelled while MUSIC takes I2S\n",
                                  static_cast<unsigned>(cancelled.type));
                }
            }
            bool ack_pause = false;
            portENTER_CRITICAL(&audio_task_state_mux);
            if (audio_pause_ack_is_current(
                    pause_request, audio_pause_request_id,
                    audio_task_state == AUDIO_TASK_PAUSE_REQUESTED))
            {
                audio_task_state = AUDIO_TASK_PAUSED;
                audio_pause_ack_id = pause_request;
                ack_pause = true;
            }
            task_state = audio_task_state;
            portEXIT_CRITICAL(&audio_task_state_mux);
            if (ack_pause && audio_task_ack_sem) xSemaphoreGive(audio_task_ack_sem);
        }

        if (task_state == AUDIO_TASK_PAUSED)
        {
            vTaskDelay(pdMS_TO_TICKS(15));
            continue;
        }

        if (task_state == AUDIO_TASK_RESUME_REQUESTED)
        {
            portENTER_CRITICAL(&audio_task_state_mux);
            if (audio_task_state == AUDIO_TASK_RESUME_REQUESTED)
                audio_task_state = AUDIO_TASK_ACTIVE;
            portEXIT_CRITICAL(&audio_task_state_mux);
        }

        AudioAsyncCommand async_cmd;
        while (audio_command_queue && xQueueReceive(audio_command_queue, &async_cmd, 0) == pdTRUE)
        {
            if (async_cmd.type == AUDIO_ASYNC_START_RECORDING)
            {
                const bool started = start_recording_transaction(async_cmd.value, async_cmd.generation);
                acknowledge_recording_command(async_cmd.generation, started);
                if (!started)
                    Serial.println("[AUDIO] stale/failed queued recording start discarded");
            }
            else if (async_cmd.type == AUDIO_ASYNC_START_PLAYBACK)
            {
                if (!start_playback_transaction(async_cmd.generation))
                    Serial.println("[AUDIO] stale/failed queued playback start discarded");
            }
            else if (async_cmd.type == AUDIO_ASYNC_SOUND_EFFECT)
                play_sound_effect_sync(static_cast<SoundEffect>(async_cmd.value));
            else if (async_cmd.type == AUDIO_ASYNC_SET_VOLUME)
            {
                audio_set_volume(async_cmd.value);
                if (async_cmd.value > 0 && audio_is_playing())
                {
                    const uint32_t session = audio_get_owner_session(AUDIO_OWNER_PLAYBACK);
                    if (session) audio_set_pa_for_session(AUDIO_OWNER_PLAYBACK, session, true);
                }
            }
        }

        // 0b. Nếu I2S đang được Music Player sử dụng hoặc driver chưa cài đặt, nhường bus hoàn toàn
        if (audio_get_current_owner() == AUDIO_OWNER_MUSIC || !audio_is_driver_installed())
        {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        // 1. Đọc luồng âm thanh đầu vào từ Microphone MEMS qua I2S RX
        bytes_read = 0;
        esp_err_t err = i2s_read(I2S_NUM_0, audio_dma_bytes, sizeof(audio_dma_bytes),
                                 &bytes_read, pdMS_TO_TICKS(25));
        if (err == ESP_OK && bytes_read > 0)
        {
            memcpy(audio_rx_bytes + rx_carry_bytes, audio_dma_bytes, bytes_read);
            const size_t total_rx_bytes = rx_carry_bytes + bytes_read;
            const size_t stereo_frames = audio_stereo_frames_from_bytes(total_rx_bytes);
            const int16_t *rx_buf = reinterpret_cast<const int16_t *>(audio_rx_bytes);
            int64_t sum_squares = 0;
            int16_t peak = 0;
            bool recording_complete = false;
            size_t completed_sample_count = 0;
            uint32_t auto_stopped_command = 0;

            const bool state_locked = audio_state_mutex &&
                                      xSemaphoreTake(audio_state_mutex, pdMS_TO_TICKS(20)) == pdTRUE;

            for (size_t frame = 0; frame < stereo_frames; ++frame)
            {
                int16_t s = rx_buf[frame * 2];
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
                if (state_locked && recording_state == RECORD_ACTIVE && psram_record_buf &&
                    recorded_samples_count < record_sample_capacity)
                {
                    psram_record_buf[recorded_samples_count++] = s;
                }
                else if (state_locked && recording_state == RECORD_ACTIVE &&
                         recorded_samples_count >= record_sample_capacity)
                {
                    recording_state = RECORD_SNAPSHOTTING;
                    completed_sample_count = recorded_samples_count;
                    recording_complete = true;
                    auto_stopped_command = active_recording_command_generation;
                }
            }

            // Tính toán RMS Level (0 - 100%) và Decibel
            if (stereo_frames > 0)
            {
                float rms = sqrtf((float)sum_squares / (float)stereo_frames);
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
                uint32_t created_gen = 0;
                const bool snapshot_ok = publish_recording_snapshot(completed_sample_count, &created_gen);
                if (snapshot_ok)
                {
                    schedule_recording_export();
                    portENTER_CRITICAL(&audio_command_mux);
                    last_completed_recording_generation = auto_stopped_command;
                    last_completed_snapshot_generation = created_gen;
                    last_completed_sample_count = completed_sample_count;
                    portEXIT_CRITICAL(&audio_command_mux);
                    if (auto_stopped_command != 0)
                    {
                        acknowledge_recording_command(auto_stopped_command, true, true,
                                                      created_gen, completed_sample_count);
                    }
                }
                audio_release_ownership(AUDIO_OWNER_RECORDER);
            }

            // 2. Phát đúng số frame theo nhịp DMA vừa nhận, không sleep cố định.
            bool playback_complete = false;
            uint32_t completed_session = 0;
            AudioRecordingLease completed_lease = {};
            if (audio_state_mutex &&
                xSemaphoreTake(audio_state_mutex, pdMS_TO_TICKS(20)) == pdTRUE)
            {
                if (playback_active && playback_lease.samples && playback_lease.sample_count > 0)
                {
                    size_t to_play = stereo_frames;
                    const size_t remaining = playback_lease.sample_count - playback_sample_idx;
                    if (to_play > remaining) to_play = remaining;
                    for (size_t i = 0; i < to_play; ++i)
                    {
                        int16_t raw_sample = playback_lease.samples[playback_sample_idx + i];
                        audio_tx_frames[i * 2]     = raw_sample; // codec is the single master volume
                        audio_tx_frames[i * 2 + 1] = raw_sample;
                    }

                    const bool write_ok = write_stereo_frames(audio_tx_frames, to_play, 40);
                    if (write_ok) playback_sample_idx += to_play;
                    if (!write_ok || playback_sample_idx >= playback_lease.sample_count)
                    {
                        playback_active = false;
                        playback_sample_idx = 0;
                        completed_lease = playback_lease;
                        playback_lease = {};
                        completed_session = playback_owner_session;
                        playback_owner_session = 0;
                        active_playback_command_generation = 0;
                        playback_complete = true;
                    }
                }
                xSemaphoreGive(audio_state_mutex);
            }
            if (playback_complete)
            {
                audio_drain_tx(300);
                audio_release_recording_lease(&completed_lease);
                audio_release_ownership_session(AUDIO_OWNER_PLAYBACK, completed_session);
            }

            const size_t consumed_rx_bytes = stereo_frames * 2U * sizeof(int16_t);
            rx_carry_bytes = audio_rx_carry_after_bytes(total_rx_bytes);
            if (rx_carry_bytes > 0)
                memmove(audio_rx_bytes, audio_rx_bytes + consumed_rx_bytes, rx_carry_bytes);
        }
        else if (err != ESP_OK)
        {
            taskYIELD();
        }
    }
}

/* =========================================================================
 * KHỞI TẠO HỆ THỐNG AUDIO DRIVER
 * ========================================================================= */
bool audio_manager_init(void)
{
    if (is_initialized) return true;

    Serial.println("\n[AUDIO] Khởi tạo hệ thống Âm thanh I2S Duplex (Mic & Speaker)...");

    if (audio_owner_mutex == nullptr) audio_owner_mutex = xSemaphoreCreateMutex();
    if (audio_i2s_tx_mutex == nullptr) audio_i2s_tx_mutex = xSemaphoreCreateMutex();
    if (audio_state_mutex == nullptr) audio_state_mutex = xSemaphoreCreateMutex();
    if (audio_codec_mutex == nullptr) audio_codec_mutex = xSemaphoreCreateMutex();
    if (audio_task_ack_sem == nullptr) audio_task_ack_sem = xSemaphoreCreateBinary();
    if (audio_command_queue == nullptr) audio_command_queue = xQueueCreate(8, sizeof(AudioAsyncCommand));
    if (!audio_owner_mutex || !audio_i2s_tx_mutex || !audio_state_mutex ||
        !audio_codec_mutex || !audio_task_ack_sem || !audio_command_queue)
    {
        Serial.println("[AUDIO] ❌ Degraded: cannot create mutex/semaphore/command queue");
        return false;
    }

    // 1. Cấu hình chân Power Amplifier (FM8002E / NS4168)
    pinMode(AUDIO_PA_PIN, OUTPUT);
    audio_set_pa_enabled(false);

    // 2. Cài đặt Driver I2S Duplex (16kHz 16-bit Master TX + RX)
    if (!audio_install_duplex_driver())
    {
        Serial.println("[AUDIO] ❌ Lỗi khởi tạo I2S Duplex Driver!");
        return false;
    }

    // 3. Khởi tạo Codec ES8311 qua I2C nếu có trên mạch
    bool has_codec = es8311_init_codec();
    if (!has_codec)
    {
        Serial.println("[AUDIO] ❌ ES8311 không sẵn sàng; vô hiệu hóa audio thay vì báo thành công giả");
        audio_uninstall_duplex_driver();
        return false;
    }
    Serial.println("[AUDIO] ✔ ES8311 Codec được cấu hình thành công");
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

    // 5. Khởi tạo FreeRTOS Task chạy trên Core 0 (Priority 3: Audio Realtime)
    BaseType_t task_ret = xTaskCreatePinnedToCore(
        audio_background_task,
        "Audio_Task",
        8 * 1024, // Codec/I2C/tone call depth plus explicit safety margin
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
    if (storage_is_available() && storage_lock(1000))
    {
        fs::FS &fs = storage_get_fs();
        if (fs.exists("/voice") || fs.mkdir("/voice"))
        {
            if (recover_recording_files_locked(fs)) recording_file_state = AUDIO_FILE_SAVED;
        }
        storage_unlock();
    }
    Serial.println("[AUDIO] Hệ thống âm thanh đã sẵn sàng trên Core 0!");
    return true;
}

static void speaker_self_test_task(void *)
{
    bool acquired = audio_request_ownership(AUDIO_OWNER_DIAGNOSTIC);
    const uint32_t session = acquired ? audio_get_owner_session(AUDIO_OWNER_DIAGNOSTIC) : 0;
    if (acquired)
    {
        const bool codec_ok = audio_codec_configure_for_stream(AUDIO_SAMPLE_RATE, 256);
        if (!codec_ok)
        {
            Serial.println("[AUDIO][SELFTEST] FAIL: codec setup/readback failed; no PCM sent");
            if (session) audio_release_ownership_session(AUDIO_OWNER_DIAGNOSTIC, session);
            else audio_release_ownership(AUDIO_OWNER_DIAGNOSTIC);
            acquired = false;
        }
    }
    if (acquired)
    {
        audio_set_pa_for_session(AUDIO_OWNER_DIAGNOSTIC, session, true);
        constexpr size_t kFrames = 128;
        constexpr size_t kTotalFrames = AUDIO_SAMPLE_RATE * 400U / 1000U;
        int16_t frames[kFrames * 2];
        float phase = 0.0f;
        const float step = 2.0f * static_cast<float>(M_PI) * 1000.0f / AUDIO_SAMPLE_RATE;
        size_t sent = 0;
        while (sent < kTotalFrames)
        {
            const size_t count = min(kFrames, kTotalFrames - sent);
            for (size_t i = 0; i < count; ++i)
            {
                const size_t absolute = sent + i;
                float envelope = 1.0f;
                if (absolute < 160) envelope = static_cast<float>(absolute) / 160.0f;
                if (kTotalFrames - absolute < 160)
                    envelope = static_cast<float>(kTotalFrames - absolute) / 160.0f;
                const int16_t sample = static_cast<int16_t>(sinf(phase) * 2800.0f * envelope);
                frames[i * 2] = sample;
                frames[i * 2 + 1] = sample;
                phase += step;
                if (phase >= 2.0f * static_cast<float>(M_PI)) phase -= 2.0f * static_cast<float>(M_PI);
            }
            if (!write_stereo_frames(frames, count, 250)) break;
            sent += count;
        }
        const bool drained = audio_drain_tx(400);
        const uint8_t volume = audio_get_volume();
        const bool pa_during_test = audio_is_pa_enabled();
        audio_release_ownership_session(AUDIO_OWNER_DIAGNOSTIC, session);
        Serial.printf("[AUDIO][SELFTEST] digital path %s Fs=%u MCLK=%u BCLK=%d LRCK=%d "
                      "requested_frames=%u written_frames=%u mute=%s volume=%u PA_during=%s PA_after=%s\n",
                      (sent == kTotalFrames && drained) ? "PASS" : "FAIL",
                      AUDIO_SAMPLE_RATE, AUDIO_SAMPLE_RATE * 256U, AUDIO_I2S_BCLK, AUDIO_I2S_WS,
                      static_cast<unsigned>(kTotalFrames), static_cast<unsigned>(sent),
                      volume == 0 ? "ON" : "OFF", volume,
                      pa_during_test ? "ON" : "OFF",
                      audio_is_pa_enabled() ? "ON" : "OFF");
    }
    if (audio_state_mutex && xSemaphoreTake(audio_state_mutex, portMAX_DELAY) == pdTRUE)
    {
        speaker_self_test_running = false;
        speaker_self_test_task_handle = nullptr;
        xSemaphoreGive(audio_state_mutex);
    }
    vTaskDelete(nullptr);
}

bool audio_speaker_self_test_async(void)
{
    if (!is_initialized || !codec_ready || !audio_state_mutex) return false;
    if (xSemaphoreTake(audio_state_mutex, pdMS_TO_TICKS(50)) != pdTRUE) return false;
    if (speaker_self_test_running)
    {
        xSemaphoreGive(audio_state_mutex);
        return false;
    }
    speaker_self_test_running = true;
    BaseType_t created = xTaskCreatePinnedToCore(speaker_self_test_task, "SpeakerTest", 3072,
                                                 nullptr, 2, &speaker_self_test_task_handle, 0);
    if (created != pdPASS)
    {
        speaker_self_test_running = false;
        speaker_self_test_task_handle = nullptr;
    }
    xSemaphoreGive(audio_state_mutex);
    return created == pdPASS;
}

bool audio_speaker_self_test_is_running(void)
{
    if (!audio_state_mutex || xSemaphoreTake(audio_state_mutex, pdMS_TO_TICKS(20)) != pdTRUE)
        return false;
    const bool running = speaker_self_test_running;
    xSemaphoreGive(audio_state_mutex);
    return running;
}

/* =========================================================================
 * BỘ TỔNG HỢP ÂM THANH (TONE & SOUNDBOARD SYNTHESIZER)
 * ========================================================================= */
void audio_play_tone(uint32_t freq_hz, uint32_t duration_ms)
{
    if (!is_initialized || freq_hz == 0 || duration_ms == 0) return;
    if (!audio_request_ownership(AUDIO_OWNER_SYSTEM)) return;
    const uint32_t session = audio_get_owner_session(AUDIO_OWNER_SYSTEM);
    if (session == 0 || !audio_codec_configure_for_stream(AUDIO_SAMPLE_RATE, 256))
    {
        if (session) audio_release_ownership_session(AUDIO_OWNER_SYSTEM, session);
        else audio_release_ownership(AUDIO_OWNER_SYSTEM);
        return;
    }
    audio_set_pa_for_session(AUDIO_OWNER_SYSTEM, session, true);

    if (audio_i2s_tx_mutex == nullptr)
    {
        audio_i2s_tx_mutex = xSemaphoreCreateMutex();
    }
    size_t total_samples = (AUDIO_SAMPLE_RATE * duration_ms) / 1000;
    const size_t CHUNK_SIZE = 128;
    int16_t buffer[CHUNK_SIZE * 2];

    float phase = 0.0f;
    float phase_step = (2.0f * (float)M_PI * (float)freq_hz) / (float)AUDIO_SAMPLE_RATE;
    const float max_amp = 12000.0f; // ES8311 is the single user-volume stage

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

        if (!write_stereo_frames(buffer, count, 250)) break;
        samples_generated += count;
    }

    audio_drain_tx(300);
    audio_release_ownership_session(AUDIO_OWNER_SYSTEM, session);
    Serial.printf("[AUDIO][TONE] freq=%u requested_frames=%u written_frames=%u\n",
                  freq_hz, static_cast<unsigned>(total_samples),
                  static_cast<unsigned>(samples_generated));
}

static void play_sound_effect_sync(SoundEffect fx)
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

bool audio_play_sound_effect(SoundEffect fx)
{
    if (!audio_command_queue || audio_task_state != AUDIO_TASK_ACTIVE ||
        audio_get_current_owner() == AUDIO_OWNER_MUSIC ||
        fx < FX_CLICK || fx > FX_XIAOZHI_WAKE) return false;
    const AudioAsyncCommand cmd = { AUDIO_ASYNC_SOUND_EFFECT, static_cast<uint8_t>(fx), 0 };
    const bool queued = xQueueSend(audio_command_queue, &cmd, 0) == pdTRUE;
    if (!queued) Serial.println("[AUDIO] Sound-effect queue full");
    return queued;
}

/* =========================================================================
 * BỘ GHI ÂM VÀ PHÁT LẠI (VOICE MEMO / PSRAM BUFFER)
 * ========================================================================= */
static bool start_recording_transaction(uint32_t max_duration_sec, uint32_t expected_generation)
{
    if (!psram_record_buf || !audio_state_mutex || max_duration_sec == 0) return false;

    if (xSemaphoreTake(audio_state_mutex, pdMS_TO_TICKS(100)) != pdTRUE) return false;
    if (expected_generation != 0 &&
        !command_generation_current(expected_generation, recording_command_generation))
    {
        xSemaphoreGive(audio_state_mutex);
        return false;
    }
    if (recording_state == RECORD_ACTIVE)
    {
        xSemaphoreGive(audio_state_mutex);
        return true;
    }
    if (recording_export_task_handle)
    {
        xSemaphoreGive(audio_state_mutex);
        return false;
    }
    if (!exclusive_start_can_claim(recording_state, RECORD_IDLE))
    {
        xSemaphoreGive(audio_state_mutex);
        return false;
    }
    const uint32_t reservation = ++recording_next_token ? recording_next_token : ++recording_next_token;
    recording_start_token = reservation;
    active_recording_command_generation = expected_generation;
    recording_state = RECORD_STARTING;
    xSemaphoreGive(audio_state_mutex);

    stop_playback_sync();
    if (!audio_request_ownership(AUDIO_OWNER_RECORDER))
    {
        if (xSemaphoreTake(audio_state_mutex, pdMS_TO_TICKS(100)) == pdTRUE)
        {
            if (recording_state == RECORD_STARTING && recording_start_token == reservation)
            {
                recording_state = RECORD_IDLE;
                active_recording_command_generation = 0;
            }
            xSemaphoreGive(audio_state_mutex);
        }
        return false;
    }

    if (xSemaphoreTake(audio_state_mutex, pdMS_TO_TICKS(100)) != pdTRUE)
    {
        if (xSemaphoreTake(audio_state_mutex, portMAX_DELAY) == pdTRUE)
        {
            if (recording_state == RECORD_STARTING && recording_start_token == reservation)
            {
                recording_state = RECORD_IDLE;
                active_recording_command_generation = 0;
            }
            xSemaphoreGive(audio_state_mutex);
        }
        audio_release_ownership(AUDIO_OWNER_RECORDER);
        return false;
    }
    if (recording_state != RECORD_STARTING || recording_start_token != reservation ||
        (expected_generation != 0 &&
         !command_generation_current(expected_generation, recording_command_generation)))
    {
        if (recording_state == RECORD_STARTING && recording_start_token == reservation)
        {
            recording_state = RECORD_IDLE;
            active_recording_command_generation = 0;
        }
        xSemaphoreGive(audio_state_mutex);
        audio_release_ownership(AUDIO_OWNER_RECORDER);
        return false;
    }
    const uint64_t requested = static_cast<uint64_t>(AUDIO_SAMPLE_RATE) * max_duration_sec;
    record_sample_capacity = requested > AUDIO_MAX_SAMPLES ? AUDIO_MAX_SAMPLES
                                                            : static_cast<uint32_t>(requested);
    recorded_samples_count = 0;
    recording_file_state = AUDIO_FILE_NONE;
    recording_state = RECORD_ACTIVE;
    xSemaphoreGive(audio_state_mutex);
    portENTER_CRITICAL(&audio_command_mux);
    last_completed_recording_generation = 0;
    last_completed_snapshot_generation = 0;
    last_completed_sample_count = 0;
    portEXIT_CRITICAL(&audio_command_mux);
    Serial.printf("[AUDIO] Bắt đầu ghi âm Mic vào PSRAM (Tối đa %u giây)...\n", max_duration_sec);
    return true;
}

bool audio_start_recording(uint32_t max_duration_sec)
{
    return start_recording_transaction(max_duration_sec, 0);
}

bool audio_start_recording_async(uint32_t max_duration_sec, uint32_t *request_id)
{
    if (!audio_command_queue || audio_task_state != AUDIO_TASK_ACTIVE ||
        audio_get_current_owner() == AUDIO_OWNER_MUSIC ||
        max_duration_sec == 0 || max_duration_sec > AUDIO_RECORD_MAX_SEC)
        return false;
    uint32_t previous = 0;
    const uint32_t issued = next_command_generation(recording_command_generation, &previous);
    const AudioAsyncCommand cmd = {
        AUDIO_ASYNC_START_RECORDING, static_cast<uint8_t>(max_duration_sec),
        issued
    };
    const bool queued = xQueueSend(audio_command_queue, &cmd, 0) == pdTRUE;
    if (!queued) rollback_command_generation(recording_command_generation, issued, previous);
    else if (request_id) *request_id = issued;
    return queued;
}

static bool stop_recording_for_generation(uint32_t cancel_through, bool discard,
                                          bool *out_snapshot_valid,
                                          uint32_t *out_snapshot_generation,
                                          size_t *out_sample_count)
{
    if (out_snapshot_valid) *out_snapshot_valid = false;
    if (out_snapshot_generation) *out_snapshot_generation = 0;
    if (out_sample_count) *out_sample_count = 0;

    if (!audio_state_mutex || xSemaphoreTake(audio_state_mutex, portMAX_DELAY) != pdTRUE)
        return false;

    // Check if recording is already stopped (e.g., auto-stop completed earlier)
    if (recording_state == RECORD_IDLE)
    {
        xSemaphoreGive(audio_state_mutex);
        if (!discard)
        {
            portENTER_CRITICAL(&audio_command_mux);
            const bool matches = (cancel_through == 0 ||
                                  audio_control_applies_to_generation(last_completed_recording_generation, cancel_through));
            if (matches && last_completed_snapshot_generation != 0)
            {
                if (out_snapshot_valid) *out_snapshot_valid = true;
                if (out_snapshot_generation) *out_snapshot_generation = last_completed_snapshot_generation;
                if (out_sample_count) *out_sample_count = last_completed_sample_count;
            }
            portEXIT_CRITICAL(&audio_command_mux);
        }
        return true;
    }

    const bool generation_matches = audio_control_applies_to_generation(
        active_recording_command_generation, cancel_through);
    if (!generation_matches)
    {
        xSemaphoreGive(audio_state_mutex);
        return true; // The requested old generation is already gone.
    }
    const bool was_recording = recording_state == RECORD_ACTIVE;
    if (recording_state == RECORD_STARTING)
    {
        ++recording_next_token;
        recording_state = RECORD_IDLE;
    }
    if (was_recording) recording_state = RECORD_SNAPSHOTTING;
    const uint32_t sample_count = recorded_samples_count;
    const uint32_t command_gen = active_recording_command_generation;
    active_recording_command_generation = 0;
    if (discard)
    {
        recording_state = RECORD_IDLE;
        recorded_samples_count = 0;
        recording_file_state = AUDIO_FILE_NONE;
        portENTER_CRITICAL(&audio_command_mux);
        last_completed_recording_generation = 0;
        last_completed_snapshot_generation = 0;
        last_completed_sample_count = 0;
        portEXIT_CRITICAL(&audio_command_mux);
    }
    xSemaphoreGive(audio_state_mutex);

    if (was_recording && !discard)
    {
        Serial.printf("[AUDIO] Đã dừng ghi âm. Thu được %u mẫu (%.2f giây)\n",
                      sample_count, (float)sample_count / AUDIO_SAMPLE_RATE);
        uint32_t created_gen = 0;
        const bool snapshot_ok = publish_recording_snapshot(sample_count, &created_gen);
        if (snapshot_ok)
        {
            schedule_recording_export();
            portENTER_CRITICAL(&audio_command_mux);
            last_completed_recording_generation = command_gen;
            last_completed_snapshot_generation = created_gen;
            last_completed_sample_count = sample_count;
            portEXIT_CRITICAL(&audio_command_mux);
            if (out_snapshot_valid) *out_snapshot_valid = true;
            if (out_snapshot_generation) *out_snapshot_generation = created_gen;
            if (out_sample_count) *out_sample_count = sample_count;
            if (command_gen != 0)
            {
                acknowledge_recording_command(command_gen, true, true, created_gen, sample_count);
            }
        }
        audio_release_ownership(AUDIO_OWNER_RECORDER);
    }
    else if (was_recording)
    {
        audio_release_ownership(AUDIO_OWNER_RECORDER);
    }
    return true;
}

void audio_stop_recording(void)
{
    (void)stop_recording_for_generation(0, false);
}

bool audio_stop_recording_async(uint32_t *request_id)
{
    if (!audio_task_handle) return false;
    uint32_t previous = 0;
    const uint32_t issued = next_command_generation(recording_command_generation, &previous);
    if (!post_recording_control(RECORD_CONTROL_STOP, issued, issued))
    {
        rollback_command_generation(recording_command_generation, issued, previous);
        return false;
    }
    if (request_id) *request_id = issued;
    return true;
}

bool audio_cancel_recording_async(uint32_t *request_id)
{
    if (!audio_task_handle) return false;
    const uint32_t issued = next_command_generation(recording_command_generation);
    post_recording_cancel_urgent(issued, issued);
    if (request_id) *request_id = issued;
    return true;
}

bool audio_cancel_recording_request_async(uint32_t expected_request_id,
                                          uint32_t *request_id)
{
    if (!audio_task_handle || expected_request_id == 0) return false;
    uint32_t issued = 0;
    portENTER_CRITICAL(&audio_command_mux);
    const bool matches = (expected_request_id == active_recording_command_generation ||
                          expected_request_id == recording_command_generation);
    if (matches)
    {
        ++recording_command_generation;
        if (recording_command_generation == 0) ++recording_command_generation;
        issued = recording_command_generation;
        if (recording_control_count <
            sizeof(recording_control_queue) / sizeof(recording_control_queue[0]))
        {
            recording_control_queue[recording_control_tail] = { RECORD_CONTROL_CANCEL, issued, expected_request_id };
            recording_control_tail = static_cast<uint8_t>(
                (recording_control_tail + 1U) %
                (sizeof(recording_control_queue) / sizeof(recording_control_queue[0])));
            ++recording_control_count;
        }
        else
        {
            if (audio_control_mailbox.recording_pending && audio_control_mailbox.recording_request_id != 0 &&
                audio_control_mailbox.recording_request_id != issued)
            {
                acknowledge_recording_command(audio_control_mailbox.recording_request_id, false);
            }
            audio_control_mailbox.recording_pending = true;
            audio_control_mailbox.recording_type = RECORD_CONTROL_CANCEL;
            audio_control_mailbox.recording_cancel_through = expected_request_id;
            audio_control_mailbox.recording_request_id = issued;
        }
    }
    portEXIT_CRITICAL(&audio_command_mux);
    if (!issued) return false;
    if (request_id) *request_id = issued;
    xTaskNotifyGive(audio_task_handle);
    return true;
}

void audio_cancel_recording(void)
{
    (void)stop_recording_for_generation(0, true);
}

bool audio_wait_recording_command_ack(uint32_t request_id, uint32_t timeout_ms,
                                      bool *operation_ok,
                                      uint32_t *snapshot_generation,
                                      size_t *sample_count)
{
    if (request_id == 0) return false;
    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    do
    {
        bool found = false;
        bool ok = false;
        bool snap_valid = false;
        uint32_t snap_gen = 0;
        size_t samples = 0;
        portENTER_CRITICAL(&audio_command_mux);
        const AudioCommandAck &slot = recording_acks[request_id %
            (sizeof(recording_acks) / sizeof(recording_acks[0]))];
        found = slot.generation == request_id;
        ok = slot.ok;
        snap_valid = slot.snapshot_valid;
        snap_gen = slot.snapshot_generation;
        samples = slot.sample_count;
        portEXIT_CRITICAL(&audio_command_mux);
        if (found)
        {
            if (operation_ok) *operation_ok = ok;
            if (snapshot_generation) *snapshot_generation = snap_valid ? snap_gen : 0;
            if (sample_count) *sample_count = snap_valid ? samples : 0;
            return true;
        }
        if (timeout_ms == 0) return false;
        vTaskDelay(pdMS_TO_TICKS(5));
    } while (static_cast<int32_t>(xTaskGetTickCount() - deadline) < 0);
    return false;
}

bool audio_is_recording(void)
{
    if (!audio_state_mutex ||
        xSemaphoreTake(audio_state_mutex, pdMS_TO_TICKS(20)) != pdTRUE) return false;
    const bool active = recording_state == RECORD_ACTIVE;
    xSemaphoreGive(audio_state_mutex);
    return active;
}

AudioRecorderStatus audio_get_recorder_status(uint32_t request_id)
{
    const bool is_rec = audio_is_recording();
    const bool owns_i2s = (audio_get_current_owner() == AUDIO_OWNER_RECORDER);

    if (request_id == 0)
    {
        if (!is_rec && !owns_i2s) return AudioRecorderStatus::STOPPED;
        return AudioRecorderStatus::BUSY;
    }

    portENTER_CRITICAL(&audio_command_mux);
    const AudioCommandAck &slot = recording_acks[request_id %
        (sizeof(recording_acks) / sizeof(recording_acks[0]))];
    const bool found = (slot.generation == request_id);
    const bool ok = slot.ok;
    const bool in_mailbox = (audio_control_mailbox.recording_pending &&
                             audio_control_mailbox.recording_request_id == request_id);
    portEXIT_CRITICAL(&audio_command_mux);

    if (found)
    {
        if (!ok) return AudioRecorderStatus::REJECTED;
        if (!is_rec && !owns_i2s) return AudioRecorderStatus::STOPPED;
        return AudioRecorderStatus::BUSY;
    }

    if (in_mailbox || is_rec || owns_i2s) return AudioRecorderStatus::BUSY;
    return AudioRecorderStatus::UNKNOWN;
}

static bool start_playback_transaction(uint32_t expected_generation)
{
    if (!psram_record_buf || !audio_state_mutex) return false;
    if (expected_generation != 0 &&
        !command_generation_current(expected_generation, playback_command_generation))
        return false;
    audio_stop_recording();

    if (xSemaphoreTake(audio_state_mutex, pdMS_TO_TICKS(100)) != pdTRUE) return false;
    if (expected_generation != 0 &&
        !command_generation_current(expected_generation, playback_command_generation))
    {
        xSemaphoreGive(audio_state_mutex);
        return false;
    }
    if (playback_active)
    {
        xSemaphoreGive(audio_state_mutex);
        return true;
    }
    if (playback_starting)
    {
        xSemaphoreGive(audio_state_mutex);
        return false;
    }
    const uint32_t reservation = ++playback_next_token ? playback_next_token : ++playback_next_token;
    playback_start_token = reservation;
    active_playback_command_generation = expected_generation;
    playback_starting = true;
    xSemaphoreGive(audio_state_mutex);

    AudioRecordingLease lease = {};
    if (!audio_acquire_recording_lease(&lease))
    {
        if (xSemaphoreTake(audio_state_mutex, pdMS_TO_TICKS(100)) == pdTRUE)
        {
            if (playback_start_token == reservation) playback_starting = false;
            if (playback_start_token == reservation) active_playback_command_generation = 0;
            xSemaphoreGive(audio_state_mutex);
        }
        return false;
    }

    if (!audio_request_ownership(AUDIO_OWNER_PLAYBACK))
    {
        audio_release_recording_lease(&lease);
        if (xSemaphoreTake(audio_state_mutex, pdMS_TO_TICKS(100)) == pdTRUE)
        {
            if (playback_start_token == reservation) playback_starting = false;
            if (playback_start_token == reservation) active_playback_command_generation = 0;
            xSemaphoreGive(audio_state_mutex);
        }
        return false;
    }

    if (xSemaphoreTake(audio_state_mutex, pdMS_TO_TICKS(100)) != pdTRUE)
    {
        if (xSemaphoreTake(audio_state_mutex, portMAX_DELAY) == pdTRUE)
        {
            if (playback_start_token == reservation) playback_starting = false;
            xSemaphoreGive(audio_state_mutex);
        }
        audio_release_ownership(AUDIO_OWNER_PLAYBACK);
        audio_release_recording_lease(&lease);
        return false;
    }
    if (!playback_starting || playback_start_token != reservation ||
        (expected_generation != 0 &&
         !command_generation_current(expected_generation, playback_command_generation)))
    {
        if (playback_start_token == reservation)
        {
            playback_starting = false;
            active_playback_command_generation = 0;
        }
        xSemaphoreGive(audio_state_mutex);
        audio_release_ownership(AUDIO_OWNER_PLAYBACK);
        audio_release_recording_lease(&lease);
        return false;
    }
    playback_sample_idx = 0;
    playback_lease = lease;
    playback_owner_session = audio_get_owner_session(AUDIO_OWNER_PLAYBACK);
    if (playback_owner_session == 0 ||
        !audio_codec_configure_for_stream(AUDIO_SAMPLE_RATE, 256))
    {
        playback_lease = {};
        playback_starting = false;
        active_playback_command_generation = 0;
        xSemaphoreGive(audio_state_mutex);
        audio_release_ownership(AUDIO_OWNER_PLAYBACK);
        audio_release_recording_lease(&lease);
        return false;
    }
    playback_active = true;
    playback_starting = false;
    audio_set_pa_for_session(AUDIO_OWNER_PLAYBACK, playback_owner_session, true);
    const size_t sample_count = lease.sample_count;
    xSemaphoreGive(audio_state_mutex);
    Serial.printf("[AUDIO] Bắt đầu phát lại đoạn ghi âm (%u mẫu)...\n",
                  static_cast<unsigned>(sample_count));
    return true;
}

bool audio_start_playback(void)
{
    return start_playback_transaction(0);
}

bool audio_start_playback_async(uint32_t *request_id)
{
    if (!audio_command_queue || audio_task_state != AUDIO_TASK_ACTIVE ||
        audio_get_current_owner() == AUDIO_OWNER_MUSIC ||
        audio_get_recorded_sample_count() == 0) return false;
    uint32_t previous = 0;
    const uint32_t issued = next_command_generation(playback_command_generation, &previous);
    const AudioAsyncCommand cmd = {
        AUDIO_ASYNC_START_PLAYBACK, 0, issued
    };
    const bool queued = xQueueSend(audio_command_queue, &cmd, 0) == pdTRUE;
    if (!queued) rollback_command_generation(playback_command_generation, issued, previous);
    else if (request_id) *request_id = issued;
    return queued;
}

static bool stop_playback_for_generation(uint32_t cancel_through)
{
    if (!audio_state_mutex || xSemaphoreTake(audio_state_mutex, portMAX_DELAY) != pdTRUE)
        return false;
    const bool generation_matches = audio_control_applies_to_generation(
        active_playback_command_generation, cancel_through);
    if (!generation_matches)
    {
        xSemaphoreGive(audio_state_mutex);
        return true;
    }
    const bool was_playing = playback_active;
    playback_starting = false;
    ++playback_next_token;
    playback_active = false;
    playback_sample_idx = 0;
    AudioRecordingLease lease = playback_lease;
    const uint32_t session = playback_owner_session;
    playback_lease = {};
    playback_owner_session = 0;
    active_playback_command_generation = 0;
    xSemaphoreGive(audio_state_mutex);
    audio_release_recording_lease(&lease);
    if (was_playing)
    {
        audio_drain_tx(250);
        audio_release_ownership_session(AUDIO_OWNER_PLAYBACK, session);
    }
    return true;
}

static void stop_playback_sync(void)
{
    (void)stop_playback_for_generation(0);
}

bool audio_stop_playback(uint32_t *request_id)
{
    if (!audio_task_handle) return false;
    uint32_t previous = 0;
    const uint32_t issued = next_command_generation(playback_command_generation, &previous);
    post_playback_stop(issued, issued);
    if (request_id) *request_id = issued;
    return true;
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
    const size_t samples = recording_state == RECORD_ACTIVE
        ? recorded_samples_count : (current_recording ? current_recording->count : 0);
    const uint32_t duration = static_cast<uint32_t>((samples * 1000ULL) / AUDIO_SAMPLE_RATE);
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
    const size_t count = recording_state == RECORD_ACTIVE || !current_recording
        ? 0 : current_recording->count;
    xSemaphoreGive(audio_state_mutex);
    return count;
}

uint32_t audio_get_recording_generation(void)
{
    if (!audio_state_mutex ||
        xSemaphoreTake(audio_state_mutex, pdMS_TO_TICKS(20)) != pdTRUE) return 0;
    const uint32_t generation = recording_generation;
    xSemaphoreGive(audio_state_mutex);
    return generation;
}

uint32_t audio_get_active_recording_command(void)
{
    portENTER_CRITICAL(&audio_command_mux);
    const uint32_t gen = active_recording_command_generation;
    portEXIT_CRITICAL(&audio_command_mux);
    return gen;
}

size_t audio_copy_recorded_samples(size_t offset, int16_t *dest, size_t max_samples)
{
    AudioRecordingLease lease = {};
    if (!audio_acquire_recording_lease(&lease)) return 0;
    const size_t count = audio_copy_recording_lease(&lease, offset, dest, max_samples);
    audio_release_recording_lease(&lease);
    return count;
}

size_t audio_copy_live_recording_samples(size_t offset, int16_t *dest,
                                         size_t max_samples, size_t *total_available,
                                         uint32_t expected_command)
{
    if (total_available) *total_available = 0;
    if (!dest || max_samples == 0 || !audio_state_mutex ||
        xSemaphoreTake(audio_state_mutex, pdMS_TO_TICKS(20)) != pdTRUE) return 0;
    const bool command_matches = (expected_command == 0 ||
                                  active_recording_command_generation == expected_command);
    const size_t available = (recording_state == RECORD_ACTIVE && command_matches)
        ? recorded_samples_count : 0;
    if (total_available) *total_available = available;
    size_t count = offset < available ? available - offset : 0;
    if (count > max_samples) count = max_samples;
    if (count > 0 && psram_record_buf) memcpy(dest, psram_record_buf + offset,
                                              count * sizeof(int16_t));
    xSemaphoreGive(audio_state_mutex);
    return count;
}

bool audio_acquire_recording_lease(AudioRecordingLease *lease, uint32_t expected_generation)
{
    if (!lease || !audio_state_mutex ||
        xSemaphoreTake(audio_state_mutex, pdMS_TO_TICKS(100)) != pdTRUE) return false;
    RecordingSnapshot *snapshot = current_recording;
    if (!snapshot || !snapshot->samples || snapshot->count == 0 ||
        (expected_generation != 0 && snapshot->generation != expected_generation))
    {
        xSemaphoreGive(audio_state_mutex);
        *lease = {};
        return false;
    }
    ++snapshot->refs;
    lease->samples = snapshot->samples;
    lease->sample_count = snapshot->count;
    lease->generation = snapshot->generation;
    lease->token = snapshot;
    xSemaphoreGive(audio_state_mutex);
    return true;
}

size_t audio_copy_recording_lease(const AudioRecordingLease *lease, size_t offset,
                                  int16_t *dest, size_t max_samples)
{
    if (!lease || !lease->token || !lease->samples || !dest || max_samples == 0 ||
        offset >= lease->sample_count) return 0;
    size_t count = lease->sample_count - offset;
    if (count > max_samples) count = max_samples;
    memcpy(dest, lease->samples + offset, count * sizeof(int16_t));
    return count;
}

void audio_release_recording_lease(AudioRecordingLease *lease)
{
    if (!lease || !lease->token) return;
    RecordingSnapshot *snapshot = static_cast<RecordingSnapshot *>(lease->token);
    RecordingSnapshot *discard = nullptr;
    if (audio_state_mutex && xSemaphoreTake(audio_state_mutex, portMAX_DELAY) == pdTRUE)
    {
        if (snapshot->refs > 0) --snapshot->refs;
        if (!snapshot->current && snapshot->refs == 0) discard = snapshot;
        xSemaphoreGive(audio_state_mutex);
    }
    *lease = {};
    free_snapshot(discard);
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
    if (!samples || count == 0 || !audio_is_driver_installed() ||
        audio_get_current_owner() == AUDIO_OWNER_NONE)
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
            stereo[i * 2] = samples[offset + i];
            stereo[i * 2 + 1] = samples[offset + i];
        }
        if (!write_stereo_frames(stereo, chunk, timeout_ms))
        {
            ok = false;
            break;
        }
        offset += chunk;
    }
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
