/**
 * @file shared_i2c_bus.cpp
 * @brief Triển khai Shared I2C Bus đồng bộ hóa giữa Touch FT6336G và Audio Codec ES8311
 */

#include "shared_i2c_bus.h"
#include "touch_transform.h"
#include <Wire.h>

static SemaphoreHandle_t s_i2c_mutex = nullptr;
static bool s_bus_initialized = false;
static bool s_touch_detected = false;
static bool s_codec_detected = false;
static portMUX_TYPE s_touch_state_mux = portMUX_INITIALIZER_UNLOCKED;
static SharedTouchSnapshot s_touch_snapshot = {};
static uint8_t s_active_touch_id = 0xFF;
static uint32_t s_last_valid_press_ms = 0;
static constexpr uint32_t kI2cErrorReleaseTimeoutMs = 60;

// Đăng ký thanh ghi FT6336G
#define FT6336_REG_TD_STATUS    0x02
#define FT6336_POINT_BYTES       6
#define FT6336_MAX_POINTS        2

bool shared_i2c_init(void)
{
    if (s_bus_initialized) return true;

    if (s_i2c_mutex == nullptr)
    {
        s_i2c_mutex = xSemaphoreCreateMutex();
    }
    if (!s_i2c_mutex)
    {
        Serial.println("[I2C] ❌ Chế độ suy giảm: không tạo được mutex bus");
        return false;
    }

    if (!shared_i2c_lock(200))
    {
        Serial.println("[I2C] ❌ Không thể lấy mutex để khởi tạo I2C bus!");
        return false;
    }

    // 1. Reset chip cảm ứng bằng phần cứng nếu chân RST được định nghĩa
#if defined(BOARD_TOUCH_RST) && (BOARD_TOUCH_RST >= 0)
    pinMode(BOARD_TOUCH_RST, OUTPUT);
    digitalWrite(BOARD_TOUCH_RST, LOW);
    delay(5);
    digitalWrite(BOARD_TOUCH_RST, HIGH);
    delay(50); // Chờ FT6336G khởi động xong
#endif

    // 2. Khởi tạo duy nhất 1 physical bus Wire với SDA/SCL và tần số chuẩn
#if defined(BOARD_TOUCH_SDA) && defined(BOARD_TOUCH_SCL)
    Wire.begin(BOARD_TOUCH_SDA, BOARD_TOUCH_SCL, 400000);
    Wire.setTimeOut(50);
#endif

    // 3. Thực hiện Probe thật các thiết bị trên bus
#if defined(BOARD_TOUCH_I2C_ADDR)
    s_touch_detected = shared_i2c_probe(BOARD_TOUCH_I2C_ADDR);
#endif

#if defined(BOARD_AUDIO_ES8311_ADDR)
    // Nếu Audio I2C dùng chung SDA/SCL với Touch (như trên ES3C28P IO16/15)
    if (BOARD_AUDIO_I2C_SDA == BOARD_TOUCH_SDA && BOARD_AUDIO_I2C_SCL == BOARD_TOUCH_SCL)
    {
        s_codec_detected = shared_i2c_probe(BOARD_AUDIO_ES8311_ADDR);
    }
    else
    {
        // Nếu khác bus (như DIYMORE), thử khởi tạo Wire1 nếu cần
        #if defined(BOARD_AUDIO_I2C_SDA) && (BOARD_AUDIO_I2C_SDA >= 0)
        Wire1.begin(BOARD_AUDIO_I2C_SDA, BOARD_AUDIO_I2C_SCL, 100000);
        Wire1.beginTransmission(BOARD_AUDIO_ES8311_ADDR);
        s_codec_detected = (Wire1.endTransmission() == 0);
        #else
        s_codec_detected = false;
        #endif
    }
#endif

    s_bus_initialized = true;
    shared_i2c_unlock();

    Serial.printf("[I2C] Bus: Shared physical bus (SDA:%d, SCL:%d, 400kHz)\n",
                  BOARD_TOUCH_SDA, BOARD_TOUCH_SCL);
    Serial.printf("[I2C] Probe FT6336 (0x%02X): %s\n",
                  BOARD_TOUCH_I2C_ADDR, s_touch_detected ? "DETECTED" : "NOT DETECTED");
    Serial.printf("[I2C] Probe ES8311 (0x%02X): %s\n",
                  BOARD_AUDIO_ES8311_ADDR, s_codec_detected ? "DETECTED" : "NOT DETECTED");

    return true;
}

bool shared_i2c_lock(uint32_t timeout_ms)
{
    if (s_i2c_mutex == nullptr)
    {
        s_i2c_mutex = xSemaphoreCreateMutex();
    }
    return s_i2c_mutex && (xSemaphoreTake(s_i2c_mutex, pdMS_TO_TICKS(timeout_ms)) == pdTRUE);
}

void shared_i2c_unlock(void)
{
    if (s_i2c_mutex != nullptr)
    {
        xSemaphoreGive(s_i2c_mutex);
    }
}

bool shared_i2c_probe(uint8_t dev_addr)
{
    Wire.beginTransmission(dev_addr);
    return (Wire.endTransmission() == 0);
}

bool shared_i2c_write_reg(uint8_t dev_addr, uint8_t reg, uint8_t val)
{
    if (!shared_i2c_lock(100)) return false;

    bool ok = false;
    if (BOARD_AUDIO_I2C_SDA == BOARD_TOUCH_SDA && BOARD_AUDIO_I2C_SCL == BOARD_TOUCH_SCL)
    {
        Wire.beginTransmission(dev_addr);
        Wire.write(reg);
        Wire.write(val);
        ok = (Wire.endTransmission() == 0);
    }
    else
    {
        #if defined(BOARD_AUDIO_I2C_SDA) && (BOARD_AUDIO_I2C_SDA >= 0)
        Wire1.beginTransmission(dev_addr);
        Wire1.write(reg);
        Wire1.write(val);
        ok = (Wire1.endTransmission() == 0);
        #endif
    }

    shared_i2c_unlock();
    return ok;
}

bool shared_i2c_read_reg(uint8_t dev_addr, uint8_t reg, uint8_t *data, size_t len)
{
    if (!data || len == 0) return false;
    if (!shared_i2c_lock(100)) return false;

    bool ok = false;
    if (BOARD_AUDIO_I2C_SDA == BOARD_TOUCH_SDA && BOARD_AUDIO_I2C_SCL == BOARD_TOUCH_SCL)
    {
        Wire.beginTransmission(dev_addr);
        Wire.write(reg);
        if (Wire.endTransmission(false) == 0)
        {
            size_t count = Wire.requestFrom((int)dev_addr, (int)len);
            if (count == len)
            {
                for (size_t i = 0; i < len; i++)
                {
                    data[i] = Wire.read();
                }
                ok = true;
            }
        }
    }
    else
    {
        #if defined(BOARD_AUDIO_I2C_SDA) && (BOARD_AUDIO_I2C_SDA >= 0)
        Wire1.beginTransmission(dev_addr);
        Wire1.write(reg);
        if (Wire1.endTransmission(false) == 0)
        {
            size_t count = Wire1.requestFrom((int)dev_addr, (int)len);
            if (count == len)
            {
                for (size_t i = 0; i < len; i++)
                {
                    data[i] = Wire1.read();
                }
                ok = true;
            }
        }
        #endif
    }

    shared_i2c_unlock();
    return ok;
}

bool shared_i2c_touch_is_detected(void)
{
    return s_touch_detected;
}

bool shared_i2c_codec_is_detected(void)
{
    return s_codec_detected;
}

static void publish_touch_snapshot(const SharedTouchSnapshot &snapshot)
{
    portENTER_CRITICAL(&s_touch_state_mux);
    s_touch_snapshot = snapshot;
    portEXIT_CRITICAL(&s_touch_state_mux);
}

bool shared_i2c_touch_read(uint16_t *x, uint16_t *y)
{
    if (!s_touch_detected || !x || !y)
    {
        return false;
    }

    SharedTouchSnapshot snapshot = {};
    snapshot.event = SHARED_TOUCH_EVENT_NONE;
    snapshot.touch_id = 0xFF;
    snapshot.timestamp_ms = millis();

    uint8_t buf[1 + FT6336_MAX_POINTS * FT6336_POINT_BYTES] = {};
    bool io_ok = false;
    if (shared_i2c_lock(20))
    {
        Wire.beginTransmission(BOARD_TOUCH_I2C_ADDR);
        Wire.write(FT6336_REG_TD_STATUS);
        if (Wire.endTransmission(false) == 0)
        {
            const size_t wanted = sizeof(buf);
            const size_t count = Wire.requestFrom((int)BOARD_TOUCH_I2C_ADDR, (int)wanted);
            if (count == wanted)
            {
                for (size_t i = 0; i < wanted; ++i) buf[i] = Wire.read();
                io_ok = true;
            }
            else
            {
                while (Wire.available()) (void)Wire.read();
            }
        }
        shared_i2c_unlock();
    }

    snapshot.io_ok = io_ok;
    if (!io_ok)
    {
        SharedTouchSnapshot previous;
        shared_i2c_touch_get_snapshot(&previous);
        if (previous.pressed && (snapshot.timestamp_ms - s_last_valid_press_ms) <= kI2cErrorReleaseTimeoutMs)
        {
            previous.io_ok = false;
            previous.timestamp_ms = snapshot.timestamp_ms;
            publish_touch_snapshot(previous);
            *x = previous.mapped_x;
            *y = previous.mapped_y;
            return true;
        }
        snapshot.sequence = previous.sequence;
        publish_touch_snapshot(snapshot);
        s_active_touch_id = 0xFF;
        return false;
    }

    snapshot.point_count = buf[0] & 0x0FU;
    SharedTouchSnapshot previous;
    shared_i2c_touch_get_snapshot(&previous);
    snapshot.sequence = previous.sequence + 1;
    if (snapshot.point_count == 0)
    {
        snapshot.sample_valid = true;
        snapshot.event = SHARED_TOUCH_EVENT_UP;
        publish_touch_snapshot(snapshot);
        s_active_touch_id = 0xFF;
        return false;
    }
    if (snapshot.point_count > FT6336_MAX_POINTS)
    {
        publish_touch_snapshot(snapshot);
        s_active_touch_id = 0xFF;
        return false;
    }

    int selected = -1;
    for (uint8_t index = 0; index < snapshot.point_count; ++index)
    {
        const size_t base = 1 + index * FT6336_POINT_BYTES;
        const uint8_t event = (buf[base] >> 6) & 0x03U;
        const uint8_t id = (buf[base + 2] >> 4) & 0x0FU;
        if (event <= SHARED_TOUCH_EVENT_CONTACT &&
            (selected < 0 || id == s_active_touch_id))
        {
            selected = index;
            if (id == s_active_touch_id) break;
        }
    }
    if (selected < 0)
    {
        publish_touch_snapshot(snapshot);
        s_active_touch_id = 0xFF;
        return false;
    }

    const size_t base = 1 + selected * FT6336_POINT_BYTES;
    snapshot.event = (buf[base] >> 6) & 0x03U;
    snapshot.touch_id = (buf[base + 2] >> 4) & 0x0FU;
    snapshot.raw_x = (uint16_t(buf[base] & 0x0FU) << 8) | buf[base + 1];
    snapshot.raw_y = (uint16_t(buf[base + 2] & 0x0FU) << 8) | buf[base + 3];

    const TouchTransformConfig transform = {
        BOARD_LCD_PANEL_WIDTH, BOARD_LCD_PANEL_HEIGHT,
        BOARD_LCD_WIDTH, BOARD_LCD_HEIGHT, BOARD_LCD_ROTATION,
        BOARD_TOUCH_SWAP_XY != 0, BOARD_TOUCH_INVERT_X != 0, BOARD_TOUCH_INVERT_Y != 0
    };
    snapshot.sample_valid = touch_transform_point(transform, snapshot.raw_x, snapshot.raw_y,
                                                  &snapshot.mapped_x, &snapshot.mapped_y);
    snapshot.pressed = snapshot.sample_valid && snapshot.event != SHARED_TOUCH_EVENT_UP;
    if (snapshot.pressed)
    {
        s_active_touch_id = snapshot.touch_id;
        s_last_valid_press_ms = snapshot.timestamp_ms;
        *x = snapshot.mapped_x;
        *y = snapshot.mapped_y;
    }
    else
    {
        s_active_touch_id = 0xFF;
    }
    publish_touch_snapshot(snapshot);
    return snapshot.pressed;
}

bool shared_i2c_touch_get_snapshot(SharedTouchSnapshot *snapshot)
{
    if (!snapshot) return false;
    portENTER_CRITICAL(&s_touch_state_mux);
    *snapshot = s_touch_snapshot;
    portEXIT_CRITICAL(&s_touch_state_mux);
    return snapshot->sequence != 0 || snapshot->timestamp_ms != 0;
}
