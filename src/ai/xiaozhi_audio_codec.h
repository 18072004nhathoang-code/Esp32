#pragma once

#include <Arduino.h>

struct OpusEncoder;
struct OpusDecoder;

class XiaozhiAudioCodec
{
public:
    XiaozhiAudioCodec();
    ~XiaozhiAudioCodec();
    bool begin(uint32_t downlink_sample_rate, char *error, size_t error_size);
    bool setDownlinkSampleRate(uint32_t sample_rate, char *error, size_t error_size);
    int encode60ms(const int16_t *pcm960, uint8_t *packet, size_t capacity);
    int decode(const uint8_t *packet, size_t packet_size, int16_t *pcm, size_t pcm_capacity);
    uint32_t downlinkSampleRate() const { return downlink_rate_; }
    void end();

private:
    OpusEncoder *encoder_;
    OpusDecoder *decoder_;
    uint32_t downlink_rate_;
};

size_t xiaozhi_resample_to_16k(const int16_t *input, size_t input_count,
                               uint32_t input_rate, int16_t *output,
                               size_t output_capacity);

