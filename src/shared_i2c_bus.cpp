/**
 * @file shared_i2c_bus.cpp
 * @brief Triển khai Shared I2C Bus đồng bộ hóa giữa Touch FT6336G và Audio Codec ES8311
 */

#include "shared_i2c_bus.h"
#include <Wire.h>
#include <Preferences.h>
#include <math.h>
#include <string.h>

static SemaphoreHandle_t s_i2c_mutex = nullptr;
static bool s_bus_initialized = false;
static bool s_touch_detected = false;
static bool s_codec_detected = false;
static TouchCalibration s_calibration = {};
static portMUX_TYPE s_touch_state_mux = portMUX_INITIALIZER_UNLOCKED;

// Đăng ký thanh ghi FT6336G
#define FT6336_REG_TD_STATUS    0x02
#define FT6336_REG_P1_XH        0x03
#define FT6336_REG_P1_XL        0x04
#define FT6336_REG_P1_YH        0x05
#define FT6336_REG_P1_YL        0x06

static bool calibration_metadata_matches(const TouchCalibration &cal)
{
    return cal.valid &&
           cal.version == TOUCH_CALIBRATION_VERSION &&
           cal.rotation == BOARD_LCD_ROTATION &&
           cal.logical_width == BOARD_LCD_WIDTH &&
           cal.logical_height == BOARD_LCD_HEIGHT &&
           cal.panel_width == BOARD_LCD_PANEL_WIDTH &&
           cal.panel_height == BOARD_LCD_PANEL_HEIGHT &&
           isfinite(cal.a) && isfinite(cal.b) && isfinite(cal.c) &&
           isfinite(cal.d) && isfinite(cal.e) && isfinite(cal.f);
}

static void load_touch_calibration(void)
{
    TouchCalibration loaded = {};
    Preferences prefs;
    if (prefs.begin("touch_cal", true))
    {
        if (prefs.getBytesLength("data") == sizeof(loaded))
        {
            prefs.getBytes("data", &loaded, sizeof(loaded));
        }
        prefs.end();
    }
    if (!calibration_metadata_matches(loaded)) loaded.valid = false;
    portENTER_CRITICAL(&s_touch_state_mux);
    s_calibration = loaded;
    portEXIT_CRITICAL(&s_touch_state_mux);
}

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
    load_touch_calibration();

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

static void mark_touch_released(void)
{
    portENTER_CRITICAL(&s_touch_state_mux);
    s_is_touched = false;
    portEXIT_CRITICAL(&s_touch_state_mux);
}

bool shared_i2c_touch_read_debug(uint16_t *raw_x, uint16_t *raw_y, uint16_t *mapped_x, uint16_t *mapped_y)
{
    portENTER_CRITICAL(&s_touch_state_mux);
    if (raw_x) *raw_x = s_last_raw_x;
    if (raw_y) *raw_y = s_last_raw_y;
    if (mapped_x) *mapped_x = s_last_mapped_x;
    if (mapped_y) *mapped_y = s_last_mapped_y;
    bool touched = s_is_touched;
    portEXIT_CRITICAL(&s_touch_state_mux);
    return touched;
}

bool shared_i2c_touch_read(uint16_t *x, uint16_t *y)
{
    if (!s_touch_detected || !x || !y)
    {
        mark_touch_released();
        return false;
    }

    if (!shared_i2c_lock(20))
    {
        mark_touch_released();
        return false;
    }

    uint8_t buf[5];
    Wire.beginTransmission(BOARD_TOUCH_I2C_ADDR);
    Wire.write(FT6336_REG_TD_STATUS);
    if (Wire.endTransmission(false) != 0)
    {
        shared_i2c_unlock();
        mark_touch_released();
        return false;
    }

    size_t count = Wire.requestFrom((int)BOARD_TOUCH_I2C_ADDR, 5);
    if (count != 5)
    {
        shared_i2c_unlock();
        mark_touch_released();
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
        mark_touch_released();
        return false;
    }

    uint16_t raw_x = ((uint16_t)(buf[1] & 0x0F) << 8) | buf[2];
    uint16_t raw_y = ((uint16_t)(buf[3] & 0x0F) << 8) | buf[4];
    if (raw_x > 4095 || raw_y > 4095)
    {
        mark_touch_released();
        return false;
    }

    TouchCalibration calibration;
    portENTER_CRITICAL(&s_touch_state_mux);
    calibration = s_calibration;
    portEXIT_CRITICAL(&s_touch_state_mux);

    int32_t mapped_x = 0;
    int32_t mapped_y = 0;
    if (calibration_metadata_matches(calibration))
    {
        mapped_x = (int32_t)lroundf(calibration.a * raw_x + calibration.b * raw_y + calibration.c);
        mapped_y = (int32_t)lroundf(calibration.d * raw_x + calibration.e * raw_y + calibration.f);
    }
    else
    {
        // Compatibility fallback only; a saved affine calibration supersedes it.
        int32_t cal_x = raw_x;
        int32_t cal_y = raw_y;
        int32_t x_extent = BOARD_LCD_PANEL_WIDTH;
        int32_t y_extent = BOARD_LCD_PANEL_HEIGHT;

#if defined(BOARD_TOUCH_SWAP_XY) && BOARD_TOUCH_SWAP_XY
        int32_t tmp = cal_x; cal_x = cal_y; cal_y = tmp;
        tmp = x_extent; x_extent = y_extent; y_extent = tmp;
#endif

#if defined(BOARD_TOUCH_INVERT_X) && BOARD_TOUCH_INVERT_X
        cal_x = (x_extent - 1) - cal_x;
#endif

#if defined(BOARD_TOUCH_INVERT_Y) && BOARD_TOUCH_INVERT_Y
        cal_y = (y_extent - 1) - cal_y;
#endif

        // Rotation transform uses native panel dimensions, never logical axes.
#ifndef BOARD_LCD_ROTATION
#define BOARD_LCD_ROTATION 0
#endif

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
    }

    // 3. Constrain vào logical dimensions (0 .. BOARD_LCD_WIDTH-1, 0 .. BOARD_LCD_HEIGHT-1)
    if (mapped_x < 0) mapped_x = 0;
    if (mapped_x >= BOARD_LCD_WIDTH)  mapped_x = BOARD_LCD_WIDTH - 1;
    if (mapped_y < 0) mapped_y = 0;
    if (mapped_y >= BOARD_LCD_HEIGHT) mapped_y = BOARD_LCD_HEIGHT - 1;

    portENTER_CRITICAL(&s_touch_state_mux);
    s_last_raw_x = raw_x;
    s_last_raw_y = raw_y;
    s_last_mapped_x = (uint16_t)mapped_x;
    s_last_mapped_y = (uint16_t)mapped_y;
    s_is_touched = true;
    portEXIT_CRITICAL(&s_touch_state_mux);

    *x = (uint16_t)mapped_x;
    *y = (uint16_t)mapped_y;
    return true;
}

static bool solve_3x3(float m[3][4], float out[3])
{
    for (int col = 0; col < 3; ++col)
    {
        int pivot = col;
        for (int row = col + 1; row < 3; ++row)
            if (fabsf(m[row][col]) > fabsf(m[pivot][col])) pivot = row;
        if (fabsf(m[pivot][col]) < 1.0e-6f) return false;
        if (pivot != col)
            for (int k = col; k < 4; ++k) { float t = m[col][k]; m[col][k] = m[pivot][k]; m[pivot][k] = t; }
        float divisor = m[col][col];
        for (int k = col; k < 4; ++k) m[col][k] /= divisor;
        for (int row = 0; row < 3; ++row)
        {
            if (row == col) continue;
            float factor = m[row][col];
            for (int k = col; k < 4; ++k) m[row][k] -= factor * m[col][k];
        }
    }
    for (int i = 0; i < 3; ++i) out[i] = m[i][3];
    return true;
}

bool shared_i2c_touch_calibrate(const TouchCalibrationPoint *points, size_t count,
                                float *rms_error, float *max_error)
{
    if (!points || count < TOUCH_CALIBRATION_POINT_COUNT) return false;
    float ata[3][3] = {};
    float atx[3] = {};
    float aty[3] = {};
    for (size_t i = 0; i < count; ++i)
    {
        const float v[3] = {points[i].raw_x, points[i].raw_y, 1.0f};
        for (int r = 0; r < 3; ++r)
        {
            atx[r] += v[r] * points[i].screen_x;
            aty[r] += v[r] * points[i].screen_y;
            for (int c = 0; c < 3; ++c) ata[r][c] += v[r] * v[c];
        }
    }
    float mx[3][4], my[3][4];
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) { mx[r][c] = ata[r][c]; my[r][c] = ata[r][c]; }
    for (int r = 0; r < 3; ++r) { mx[r][3] = atx[r]; my[r][3] = aty[r]; }
    float cx[3], cy[3];
    if (!solve_3x3(mx, cx) || !solve_3x3(my, cy)) return false;

    float sum_sq = 0.0f;
    float worst = 0.0f;
    for (size_t i = 0; i < count; ++i)
    {
        float px = cx[0] * points[i].raw_x + cx[1] * points[i].raw_y + cx[2];
        float py = cy[0] * points[i].raw_x + cy[1] * points[i].raw_y + cy[2];
        float ex = px - points[i].screen_x;
        float ey = py - points[i].screen_y;
        float err = sqrtf(ex * ex + ey * ey);
        sum_sq += err * err;
        if (err > worst) worst = err;
    }
    float rms = sqrtf(sum_sq / count);
    if (rms_error) *rms_error = rms;
    if (max_error) *max_error = worst;
    if (rms > 8.0f || worst > 12.0f) return false;

    TouchCalibration result = {};
    result.valid = true;
    result.version = TOUCH_CALIBRATION_VERSION;
    result.rotation = BOARD_LCD_ROTATION;
    result.logical_width = BOARD_LCD_WIDTH;
    result.logical_height = BOARD_LCD_HEIGHT;
    result.panel_width = BOARD_LCD_PANEL_WIDTH;
    result.panel_height = BOARD_LCD_PANEL_HEIGHT;
    result.a = cx[0]; result.b = cx[1]; result.c = cx[2];
    result.d = cy[0]; result.e = cy[1]; result.f = cy[2];
    result.rms_error = rms;
    result.max_error = worst;

    Preferences prefs;
    bool persisted = false;
    if (prefs.begin("touch_cal", false))
    {
        persisted = prefs.putBytes("data", &result, sizeof(result)) == sizeof(result);
        prefs.end();
    }
    if (!persisted) return false;
    portENTER_CRITICAL(&s_touch_state_mux);
    s_calibration = result;
    portEXIT_CRITICAL(&s_touch_state_mux);
    return true;
}

TouchCalibration shared_i2c_touch_get_calibration(void)
{
    TouchCalibration result;
    portENTER_CRITICAL(&s_touch_state_mux);
    result = s_calibration;
    portEXIT_CRITICAL(&s_touch_state_mux);
    result.valid = calibration_metadata_matches(result);
    return result;
}

void shared_i2c_touch_reset_calibration(void)
{
    TouchCalibration empty = {};
    portENTER_CRITICAL(&s_touch_state_mux);
    s_calibration = empty;
    portEXIT_CRITICAL(&s_touch_state_mux);
    Preferences prefs;
    if (prefs.begin("touch_cal", false))
    {
        prefs.clear();
        prefs.end();
    }
}
