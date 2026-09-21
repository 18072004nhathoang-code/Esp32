#include "xiaozhi_audio_codec.h"

#include <opus.h>

XiaozhiAudioCodec::XiaozhiAudioCodec()
    : encoder_(nullptr), decoder_(nullptr), downlink_rate_(0) {}

XiaozhiAudioCodec::~XiaozhiAudioCodec() { end(); }

bool XiaozhiAudioCodec::begin(uint32_t downlink_sample_rate, char *error, size_t error_size)
{
    end();
    int code = OPUS_OK;
    encoder_ = opus_encoder_create(16000, 1, OPUS_APPLICATION_VOIP, &code);
    if (!encoder_ || code != OPUS_OK)
    {
        if (error && error_size) snprintf(error, error_size, "Opus encoder lỗi %d", code);
        end();
        return false;
    }
    if (opus_encoder_ctl(encoder_, OPUS_SET_BITRATE(24000)) != OPUS_OK ||
        opus_encoder_ctl(encoder_, OPUS_SET_COMPLEXITY(5)) != OPUS_OK ||
        opus_encoder_ctl(encoder_, OPUS_SET_VBR(1)) != OPUS_OK)
    {
        if (error && error_size) strlcpy(error, "Không cấu hình được Opus encoder", error_size);
        end();
        return false;
    }
    return setDownlinkSampleRate(downlink_sample_rate, error, error_size);
}

bool XiaozhiAudioCodec::setDownlinkSampleRate(uint32_t sample_rate,
                                               char *error, size_t error_size)
{
    if (sample_rate != 8000 && sample_rate != 12000 && sample_rate != 16000 &&
        sample_rate != 24000 && sample_rate != 48000)
    {
        if (error && error_size) strlcpy(error, "Tần số Opus downlink không hỗ trợ", error_size);
        return false;
    }
    // Cấu hình OpusDecoder luôn giải mã ra 16000 Hz để khớp trực tiếp với phần cứng I2S DAC ES8311.
    // Thư viện libopus (RFC 6716) tích hợp sẵn bộ lọc sinc đa pha chất lượng cao (>64dB stopband),
    // tự động chuyển đổi từ bất kỳ tần số đầu vào nào (8k/12k/16k/24k/48k) sang 16kHz sạch sẽ,
    // loại bỏ hoàn toàn hiện tượng méo hài (aliasing) và tiếng nổ vi mô giữa các frame (phase discontinuity).
    if (decoder_ && downlink_rate_ == 16000) return true;
    if (decoder_) opus_decoder_destroy(decoder_);
    decoder_ = nullptr;
    int code = OPUS_OK;
    decoder_ = opus_decoder_create(16000, 1, &code);
    if (!decoder_ || code != OPUS_OK)
    {
        if (error && error_size) snprintf(error, error_size, "Opus decoder lỗi %d", code);
        decoder_ = nullptr;
        downlink_rate_ = 0;
        return false;
    }
    downlink_rate_ = 16000;
    return true;
}

int XiaozhiAudioCodec::encode60ms(const int16_t *pcm960, uint8_t *packet, size_t capacity)
{
    if (!encoder_ || !pcm960 || !packet || capacity == 0 || capacity > 0x7fffffffU) return OPUS_BAD_ARG;
    return opus_encode(encoder_, pcm960, 960, packet, static_cast<opus_int32>(capacity));
}

int XiaozhiAudioCodec::decode(const uint8_t *packet, size_t packet_size,
                              int16_t *pcm, size_t pcm_capacity)
{
    if (!decoder_ || !packet || packet_size == 0 || !pcm || pcm_capacity == 0 ||
        packet_size > 0x7fffffffU || pcm_capacity > 0x7fffffffU) return OPUS_BAD_ARG;
    return opus_decode(decoder_, packet, static_cast<opus_int32>(packet_size), pcm,
                       static_cast<int>(pcm_capacity), 0);
}

void XiaozhiAudioCodec::end()
{
    if (encoder_) opus_encoder_destroy(encoder_);
    if (decoder_) opus_decoder_destroy(decoder_);
    encoder_ = nullptr;
    decoder_ = nullptr;
    downlink_rate_ = 0;
}

size_t xiaozhi_resample_to_16k(const int16_t *input, size_t input_count,
                               uint32_t input_rate, int16_t *output,
                               size_t output_capacity)
{
    if (!input || input_count == 0 || !output || output_capacity == 0 || input_rate == 0) return 0;
    if (input_rate == 16000)
    {
        const size_t count = input_count < output_capacity ? input_count : output_capacity;
        memcpy(output, input, count * sizeof(int16_t));
        return count;
    }
    const uint64_t expected = (static_cast<uint64_t>(input_count) * 16000U) / input_rate;
    size_t output_count = expected > output_capacity ? output_capacity : static_cast<size_t>(expected);
    if (output_count == 0) return 0;
    for (size_t i = 0; i < output_count; ++i)
    {
        const uint64_t position = static_cast<uint64_t>(i) * input_rate;
        const size_t index = static_cast<size_t>(position / 16000U);
        const uint32_t fraction = static_cast<uint32_t>(position % 16000U);
        if (index + 1 >= input_count) output[i] = input[input_count - 1];
        else
        {
            const int32_t a = input[index];
            const int32_t b = input[index + 1];
            output[i] = static_cast<int16_t>(a + ((b - a) * static_cast<int32_t>(fraction)) / 16000);
        }
    }
    return output_count;
}
