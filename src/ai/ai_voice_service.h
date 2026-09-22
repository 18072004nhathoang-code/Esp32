/**
 * @file ai_voice_service.h
 * @brief Phân hệ AI Voice: thu I2S -> gateway STT/LLM/TTS -> loa ngoài
 * Chạy nền trên FreeRTOS Core 0 chuyên dụng cho ESP32-S3
 */

#pragma once

#include <Arduino.h>

#define AI_MAX_CHAT_MESSAGES   32
#define AI_MAX_TEXT_LEN        512

enum AIVoiceState
{
    AI_STATE_IDLE = 0,         // Sẵn sàng chờ lệnh
    AI_STATE_STARTING,         // Đang tạm dừng nhạc và chuẩn bị microphone
    AI_STATE_LISTENING,        // Đang thu âm từ Micro MEMS
    AI_STATE_PROCESSING,       // Đang gửi dữ liệu và đợi gateway STT/LLM
    AI_STATE_SPEAKING,         // Đang phát giọng nói phản hồi ra Loa ngoài
    AI_STATE_CANCELING,        // Đang chờ worker hủy request và nhả tài nguyên
    AI_STATE_NEEDS_USER_INPUT, // Thiếu endpoint/token/CA bắt buộc
    AI_STATE_ERROR             // Tài nguyên hệ thống không sẵn sàng
};

enum class AiVoiceStopReason : uint8_t
{
    USER_RELEASE = 0,
    PRESS_LOST,
    APP_CLOSE,
    CANCEL,
    MAX_DURATION,
    AUDIO_ERROR,
    TOO_SHORT
};

inline const char *ai_voice_stop_reason_str(AiVoiceStopReason reason)
{
    switch (reason)
    {
        case AiVoiceStopReason::USER_RELEASE: return "USER_RELEASE";
        case AiVoiceStopReason::PRESS_LOST:   return "PRESS_LOST";
        case AiVoiceStopReason::APP_CLOSE:    return "APP_CLOSE";
        case AiVoiceStopReason::CANCEL:       return "CANCEL";
        case AiVoiceStopReason::MAX_DURATION: return "MAX_DURATION";
        case AiVoiceStopReason::AUDIO_ERROR:  return "AUDIO_ERROR";
        case AiVoiceStopReason::TOO_SHORT:    return "TOO_SHORT";
        default:                              return "UNKNOWN";
    }
}

struct ChatMessage
{
    bool is_user;              // true: Người dùng, false: AI Assistant
    char text[AI_MAX_TEXT_LEN];
    uint32_t timestamp_sec;
    uint32_t id;               // ID tin nhắn tuần tự duy nhất
};

/**
 * @brief Khởi tạo phân hệ AI Voice Service và Task FreeRTOS trên Core 0
 */
bool ai_voice_init(void);
bool ai_voice_is_available(void);
const char *ai_voice_get_last_error(void);
bool ai_voice_copy_last_error(char *out, size_t out_size);

/**
 * @brief Bắt đầu ghi âm từ Micro MEMS khi nhấn giữ nút Push-to-Talk
 */
bool ai_voice_start_recording(void);

/**
 * @brief Dừng ghi âm khi thả nút và gửi âm thanh lên luồng xử lý AI
 */
bool ai_voice_stop_and_process(AiVoiceStopReason reason = AiVoiceStopReason::USER_RELEASE);

/** @brief Hủy lần thu hiện tại khi UI đóng, không gửi dữ liệu lên mạng. */
void ai_voice_cancel(AiVoiceStopReason reason = AiVoiceStopReason::CANCEL);

/** @brief Lấy generation hiện tại của phiên ghi âm/xử lý. */
uint32_t ai_voice_get_active_generation(void);

/**
 * @brief Lấy trạng thái hiện tại của AI Voice Assistant
 */
AIVoiceState ai_voice_get_state(void);

/**
 * @brief Lấy chuỗi mô tả trạng thái (VD: "Đang lắng nghe...", "Đang suy nghĩ...")
 */
const char* ai_voice_get_state_text(void);
/** @brief Copy a coherent state description under the Xiaozhi mutex. */
bool ai_voice_copy_state_text(char *out, size_t out_size);

/**
 * @brief Lấy số lượng tin nhắn trong lịch sử hội thoại
 */
int ai_voice_get_message_count(void);
uint32_t ai_voice_get_history_revision(void);

/**
 * @brief Lấy bản sao an toàn của tin nhắn theo chỉ số index dưới khóa Mutex
 */
bool ai_voice_get_message_copy(int index, ChatMessage *out_msg);

/**
 * @brief Thêm một tin nhắn vào lịch sử hội thoại
 */
void ai_voice_add_message(bool is_user, const char *text);

/**
 * @brief Xóa toàn bộ lịch sử trò chuyện
 */
void ai_voice_clear_history(void);
uint32_t ai_voice_get_clear_count(void);

/**
 * @brief Phát âm thanh phản hồi từ văn bản qua TTS ra Loa ngoài
 */
bool ai_voice_play_tts(const char *text);

/** Xiaozhi activation is asynchronous and never blocks LVGL. */
bool ai_voice_get_activation(char *code, size_t code_size,
                             char *message, size_t message_size);
bool ai_voice_retry_activation(void);
bool ai_voice_cancel_activation(void);

/**
 * @brief Pre-connect WebSocket in background when entering AI Voice app.
 * Ensures connection and session handshake are ready before user touches PTT button.
 */
bool ai_voice_preconnect(void);

/**
 * @brief Inform AI voice service that app was closed.
 * Closes idle connection to release sockets and memory for other apps.
 */
void ai_voice_on_app_closed(void);

/**
 * @brief Check if WebSocket is warm-connected and ready for instant PTT speech.
 */
bool ai_voice_is_connected(void);

/** Firmware-side regression for the exact ArduinoJson codec used by requests. */
bool ai_voice_json_regression_test(void);
