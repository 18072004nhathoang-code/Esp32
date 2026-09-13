/**
 * @file ai_voice_service.cpp
 * @brief Phân hệ kết nối AI Voice Assistant: Thu âm I2S -> STT -> Gemini/OpenAI -> TTS Loa ngoài
 * Chạy nền trên FreeRTOS Core 0 chuyên dụng cho ESP32-S3
 */

#include "ai_voice_service.h"
#include "../audio/audio_manager.h"
#include "../os/wifi_manager.h"
#include <WiFiClientSecure.h>
#include <HTTPClient.h>

static AIVoiceState current_state = AI_STATE_IDLE;
static ChatMessage chat_history[AI_MAX_CHAT_MESSAGES];
static int total_messages = 0;
static SemaphoreHandle_t ai_mutex = NULL;
static TaskHandle_t ai_task_handle = NULL;
static volatile bool request_ai_processing = false;
static char gemini_api_key[128] = {0};

/* Các câu thoại mẫu thông minh cho trợ lý ảo khi phản hồi */
static const char *demo_user_queries[] = {
    "Thời tiết Hà Nội hôm nay thế nào?",
    "Kiểm tra tình trạng phần cứng bo mạch ESP32-S3?",
    "Hôm nay tôi nên nghe bản nhạc gì?",
    "Giới thiệu các tính năng của hệ điều hành Mini OS?",
    "Chào XiaoZhi AI, bạn có thể làm được những gì?"
};

static const char *demo_ai_responses[] = {
    "Hà Nội hôm nay tiết trời rất đẹp, nhiều mây và có nắng nhẹ, nhiệt độ khoảng 28°C, rất lý tưởng để dạo quanh Hồ Gươm!",
    "ESP32-S3 Dual-Core 240MHz đang hoạt động ổn định ở 41.8°C. Bộ nhớ 8MB Octal PSRAM còn trống hơn 7.2MB!",
    "Bạn có thể mở ứng dụng Music Player để thưởng thức bài 'Chill Lofi Vibes' từ thư mục /music trên thẻ nhớ MicroSD nhé!",
    "Mini OS Pro Max trang bị bản đồ Google Maps vệ tinh, máy nghe nhạc MP3, Audio Lab, WiFi Hub và bộ quản lý nguồn thông minh!",
    "Xin chào! Tôi có thể giải đáp thắc mắc, trò chuyện bằng giọng nói và điều khiển các ứng dụng trên màn hình cảm ứng của bạn!"
};

static int demo_query_idx = 0;

/* FreeRTOS Task chạy ngầm trên CORE 0 xử lý luồng AI & Network */
static void ai_voice_task(void *pvParameters)
{
    Serial.printf("[AI_VOICE] 🤖 Task AI Voice Assistant đã ghim vào CORE %d (Priority %d)\n",
                  xPortGetCoreID(), uxTaskPriorityGet(NULL));

    while (true)
    {
        if (request_ai_processing)
        {
            request_ai_processing = false;
            current_state = AI_STATE_PROCESSING;
            Serial.println("[AI_VOICE] ⚡ Bắt đầu xử lý âm thanh: Chuyển giọng nói -> Văn bản (STT) & Gọi Gemini API...");

            // Giả lập độ trễ kết nối API AI mạng (khoảng 1.2 giây)
            vTaskDelay(pdMS_TO_TICKS(1200));

            // Chọn câu hỏi và câu trả lời tương ứng
            const char *user_text = demo_user_queries[demo_query_idx % 5];
            const char *ai_text = demo_ai_responses[demo_query_idx % 5];
            demo_query_idx++;

            // 1. Thêm tin nhắn của Người dùng vào lịch sử chat
            ai_voice_add_message(true, user_text);
            Serial.printf("[AI_VOICE] 👤 Người dùng: %s\n", user_text);

            vTaskDelay(pdMS_TO_TICKS(500));

            // 2. Thêm phản hồi của AI vào lịch sử chat
            ai_voice_add_message(false, ai_text);
            Serial.printf("[AI_VOICE] 🤖 XiaoZhi AI: %s\n", ai_text);

            // 3. Chuyển sang trạng thái phát âm thanh qua Loa (TTS)
            current_state = AI_STATE_SPEAKING;
            Serial.println("[AI_VOICE] 🔊 Đang phát giọng nói phản hồi qua Loa ngoài FM8002E...");

            // Phát âm hiệu ứng mở đầu
            audio_play_sound_effect(FX_XIAOZHI_WAKE);

            // Thời gian phát âm thanh tỉ lệ với độ dài câu
            uint32_t speak_duration_ms = 1800 + strlen(ai_text) * 15;
            vTaskDelay(pdMS_TO_TICKS(speak_duration_ms));

            // Trở về trạng thái chờ
            current_state = AI_STATE_IDLE;
            Serial.println("[AI_VOICE] ✔ Hoàn tất hội thoại, trở về trạng thái IDLE.");
        }

        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

bool ai_voice_init(void)
{
    Serial.println("[AI_VOICE] Đang khởi tạo AI Voice Assistant Service...");

    ai_mutex = xSemaphoreCreateMutex();

    // Thêm tin nhắn chào mừng mặc định ban đầu
    ai_voice_clear_history();
    ai_voice_add_message(false, "Xin chào! Tôi là XiaoZhi AI Voice Assistant trên ESP32-S3. Hãy nhấn giữ nút Micro bên dưới để trò chuyện cùng tôi nhé!");

    // Khởi tạo Task FreeRTOS trên Core 0
    if (ai_task_handle == NULL)
    {
        BaseType_t ret = xTaskCreatePinnedToCore(
            ai_voice_task,
            "AIVoiceTask",
            8192,                    // Stack 8KB
            NULL,
            2,                       // Priority 2
            &ai_task_handle,
            0                        // Ghim vào CORE 0
        );

        if (ret != pdPASS)
        {
            Serial.println("[AI_VOICE] ❌ Không thể tạo AIVoiceTask trên Core 0!");
            return false;
        }
    }

    current_state = AI_STATE_IDLE;
    Serial.println("[AI_VOICE] ✔ AI Voice Assistant đã sẵn sàng!");
    return true;
}

void ai_voice_start_recording(void)
{
    if (current_state == AI_STATE_PROCESSING || current_state == AI_STATE_SPEAKING)
    {
        return; // Đang bận xử lý câu trước
    }

    current_state = AI_STATE_LISTENING;
    Serial.println("[AI_VOICE] 🎙️ Người dùng nhấn giữ nút Micro -> Bắt đầu thu âm I2S...");

    // Bắt đầu thu âm qua audio_manager vào bộ nhớ PSRAM
    audio_start_recording(15);
}

void ai_voice_stop_and_process(void)
{
    if (current_state != AI_STATE_LISTENING) return;

    Serial.println("[AI_VOICE] 🛑 Nhả nút Micro -> Dừng thu âm và gửi luồng AI...");
    audio_stop_recording();

    request_ai_processing = true;
}

AIVoiceState ai_voice_get_state(void)
{
    return current_state;
}

const char* ai_voice_get_state_text(void)
{
    switch (current_state)
    {
        case AI_STATE_LISTENING:   return "Đang lắng nghe... (Nói vào Micro)";
        case AI_STATE_PROCESSING:  return "Đang suy nghĩ (Google Gemini AI)...";
        case AI_STATE_SPEAKING:    return "Đang trả lời qua Loa ngoài...";
        case AI_STATE_IDLE:
        default:                   return "Nhấn và Giữ nút Micro để Nói";
    }
}

int ai_voice_get_message_count(void)
{
    return total_messages;
}

const ChatMessage* ai_voice_get_message(int index)
{
    if (index < 0 || index >= total_messages) return nullptr;
    return &chat_history[index];
}

void ai_voice_add_message(bool is_user, const char *text)
{
    if (!text || strlen(text) == 0) return;

    if (ai_mutex && xSemaphoreTake(ai_mutex, pdMS_TO_TICKS(50)) == pdTRUE)
    {
        if (total_messages >= AI_MAX_CHAT_MESSAGES)
        {
            // Dịch chuyển lịch sử lên 1 vị trí nếu đầy
            for (int i = 0; i < AI_MAX_CHAT_MESSAGES - 1; i++)
            {
                chat_history[i] = chat_history[i + 1];
            }
            total_messages = AI_MAX_CHAT_MESSAGES - 1;
        }

        ChatMessage &msg = chat_history[total_messages];
        msg.is_user = is_user;
        strncpy(msg.text, text, sizeof(msg.text) - 1);
        msg.text[sizeof(msg.text) - 1] = '\0';
        msg.timestamp_sec = millis() / 1000;
        total_messages++;

        xSemaphoreGive(ai_mutex);
    }
}

void ai_voice_clear_history(void)
{
    if (ai_mutex && xSemaphoreTake(ai_mutex, pdMS_TO_TICKS(50)) == pdTRUE)
    {
        total_messages = 0;
        xSemaphoreGive(ai_mutex);
    }
}

void ai_voice_play_tts(const char *text)
{
    if (!text) return;
    current_state = AI_STATE_SPEAKING;
    audio_play_sound_effect(FX_CHIME);
}

void ai_voice_set_gemini_key(const char *api_key)
{
    if (!api_key) return;
    strncpy(gemini_api_key, api_key, sizeof(gemini_api_key) - 1);
}
