/**
 * @file ai_voice_service.h
 * @brief Phân hệ kết nối AI Voice Assistant: Thu âm I2S -> STT -> Gemini/OpenAI -> TTS Loa ngoài
 * Chạy nền trên FreeRTOS Core 0 chuyên dụng cho ESP32-S3
 */

#pragma once

#include <Arduino.h>

#define AI_MAX_CHAT_MESSAGES   32
#define AI_MAX_TEXT_LEN        512

enum AIVoiceState
{
    AI_STATE_IDLE = 0,         // Sẵn sàng chờ lệnh
    AI_STATE_LISTENING,        // Đang thu âm từ Micro MEMS
    AI_STATE_PROCESSING,       // Đang gửi dữ liệu và đợi Gemini AI suy nghĩ
    AI_STATE_SPEAKING          // Đang phát giọng nói phản hồi ra Loa ngoài
};

struct ChatMessage
{
    bool is_user;              // true: Người dùng, false: AI Assistant
    char text[AI_MAX_TEXT_LEN];
    uint32_t timestamp_sec;
};

/**
 * @brief Khởi tạo phân hệ AI Voice Service và Task FreeRTOS trên Core 0
 */
bool ai_voice_init(void);

/**
 * @brief Bắt đầu ghi âm từ Micro MEMS khi nhấn giữ nút Push-to-Talk
 */
void ai_voice_start_recording(void);

/**
 * @brief Dừng ghi âm khi thả nút và gửi âm thanh lên luồng xử lý AI
 */
void ai_voice_stop_and_process(void);

/**
 * @brief Lấy trạng thái hiện tại của AI Voice Assistant
 */
AIVoiceState ai_voice_get_state(void);

/**
 * @brief Lấy chuỗi mô tả trạng thái (VD: "Đang lắng nghe...", "Đang suy nghĩ...")
 */
const char* ai_voice_get_state_text(void);

/**
 * @brief Lấy số lượng tin nhắn trong lịch sử hội thoại
 */
int ai_voice_get_message_count(void);

/**
 * @brief Lấy tin nhắn theo chỉ số index
 */
const ChatMessage* ai_voice_get_message(int index);

/**
 * @brief Thêm một tin nhắn vào lịch sử hội thoại
 */
void ai_voice_add_message(bool is_user, const char *text);

/**
 * @brief Xóa toàn bộ lịch sử trò chuyện
 */
void ai_voice_clear_history(void);

/**
 * @brief Phát âm thanh phản hồi từ văn bản qua TTS ra Loa ngoài
 */
void ai_voice_play_tts(const char *text);

/**
 * @brief Cấu hình khóa API Google Gemini
 */
void ai_voice_set_gemini_key(const char *api_key);
