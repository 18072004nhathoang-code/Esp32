/**
 * @file shared_i2c_bus.cpp
 * @brief Triển khai Shared I2C Bus đồng bộ hóa giữa Touch FT6336G và Audio Codec ES8311
 */

#include "shared_i2c_bus.h"
#include <Wire.h>

static SemaphoreHandle_t s_i2c_mutex = nullptr;
static bool s_bus_initialized = false;
static bool s_touch_detected = false;
static bool s_codec_detected = false;

// Đăng ký thanh ghi FT6336G
#define FT6336_REG_TD_STATUS    0x02
#define FT6336_REG_P1_XH        0x03
#define FT6336_REG_P1_XL        0x04
#define FT6336_REG_P1_YH        0x05
#define FT6336_REG_P1_YL        0x06

bool shared_i2c_init(void)
{
    if (s_bus_initialized) return true;

    if (s_i2c_mutex == nullptr)
    {
        s_i2c_mutex = xSemaphoreCreateMutex();
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
    return (xSemaphoreTake(s_i2c_mutex, pdMS_TO_TICKS(timeout_ms)) == pdTRUE);
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

static uint16_t s_last_raw_x = 0;
static uint16_t s_last_raw_y = 0;
static uint16_t s_last_mapped_x = 0;
static uint16_t s_last_mapped_y = 0;
static bool s_is_touched = false;

bool shared_i2c_touch_read_debug(uint16_t *raw_x, uint16_t *raw_y, uint16_t *mapped_x, uint16_t *mapped_y)
{
    if (raw_x) *raw_x = s_last_raw_x;
    if (raw_y) *raw_y = s_last_raw_y;
    if (mapped_x) *mapped_x = s_last_mapped_x;
    if (mapped_y) *mapped_y = s_last_mapped_y;
    return s_is_touched;
}

bool shared_i2c_touch_read(uint16_t *x, uint16_t *y)
{
    if (!s_touch_detected || !x || !y) return false;

    if (!shared_i2c_lock(20)) return false;

    uint8_t buf[5];
    Wire.beginTransmission(BOARD_TOUCH_I2C_ADDR);
    Wire.write(FT6336_REG_TD_STATUS);
    if (Wire.endTransmission(false) != 0)
    {
        shared_i2c_unlock();
        return false;
    }

    size_t count = Wire.requestFrom((int)BOARD_TOUCH_I2C_ADDR, 5);
    if (count != 5)
    {
        shared_i2c_unlock();
        return false;
    }

    for (int i = 0; i < 5; i++)
    {
        buf[i] = Wire.read();
    }
    shared_i2c_unlock();

    uint8_t touches = buf[0] & 0x0F;
    if (touches == 0 || touches > 2)
    {
        s_is_touched = false;
        return false;
    }

    uint16_t raw_x = ((uint16_t)(buf[1] & 0x0F) << 8) | buf[2];
    uint16_t raw_y = ((uint16_t)(buf[3] & 0x0F) << 8) | buf[4];

    // 1. Áp dụng calibration trước rotation
    int32_t cal_x = raw_x;
    int32_t cal_y = raw_y;

#if defined(BOARD_TOUCH_SWAP_XY) && BOARD_TOUCH_SWAP_XY
    int32_t tmp = cal_x; cal_x = cal_y; cal_y = tmp;
#endif

#if defined(BOARD_TOUCH_INVERT_X) && BOARD_TOUCH_INVERT_X
    cal_x = (BOARD_LCD_PANEL_WIDTH - 1) - cal_x;
#endif

#if defined(BOARD_TOUCH_INVERT_Y) && BOARD_TOUCH_INVERT_Y
    cal_y = (BOARD_LCD_PANEL_HEIGHT - 1) - cal_y;
#endif

    // 2. Transform theo BOARD_LCD_ROTATION dựa trên native panel dimensions
#ifndef BOARD_LCD_ROTATION
#define BOARD_LCD_ROTATION 0
#endif

    int32_t mapped_x = cal_x;
    int32_t mapped_y = cal_y;

#if (BOARD_LCD_ROTATION == 0)
    mapped_x = cal_x;
    mapped_y = cal_y;
#elif (BOARD_LCD_ROTATION == 1)
    mapped_x = cal_y;
    mapped_y = BOARD_LCD_PANEL_WIDTH - 1 - cal_x;
#elif (BOARD_LCD_ROTATION == 2)
    mapped_x = BOARD_LCD_PANEL_WIDTH - 1 - cal_x;
    mapped_y = BOARD_LCD_PANEL_HEIGHT - 1 - cal_y;
#elif (BOARD_LCD_ROTATION == 3)
    mapped_x = BOARD_LCD_PANEL_HEIGHT - 1 - cal_y;
    mapped_y = cal_x;
#endif

    // 3. Constrain vào logical dimensions (0 .. BOARD_LCD_WIDTH-1, 0 .. BOARD_LCD_HEIGHT-1)
    if (mapped_x < 0) mapped_x = 0;
    if (mapped_x >= BOARD_LCD_WIDTH)  mapped_x = BOARD_LCD_WIDTH - 1;
    if (mapped_y < 0) mapped_y = 0;
    if (mapped_y >= BOARD_LCD_HEIGHT) mapped_y = BOARD_LCD_HEIGHT - 1;

    s_last_raw_x = raw_x;
    s_last_raw_y = raw_y;
    s_last_mapped_x = (uint16_t)mapped_x;
    s_last_mapped_y = (uint16_t)mapped_y;
    s_is_touched = true;

    *x = (uint16_t)mapped_x;
    *y = (uint16_t)mapped_y;
    return true;
}
