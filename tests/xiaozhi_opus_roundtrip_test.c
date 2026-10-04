#include <stdint.h>
#include <stdio.h>

#include "opus.h"

int main(void)
{
    int error = OPUS_OK;
    OpusEncoder *encoder = opus_encoder_create(
        16000, 1, OPUS_APPLICATION_VOIP, &error);
    if (!encoder || error != OPUS_OK) return 1;
    if (opus_encoder_ctl(encoder, OPUS_SET_BITRATE(24000)) != OPUS_OK ||
        opus_encoder_ctl(encoder, OPUS_SET_COMPLEXITY(5)) != OPUS_OK ||
        opus_encoder_ctl(encoder, OPUS_SET_VBR(1)) != OPUS_OK)
        return 2;

    OpusDecoder *decoder = opus_decoder_create(16000, 1, &error);
    if (!decoder || error != OPUS_OK) return 3;

    int16_t input[960];
    int16_t output[1920];
    uint32_t pattern = 0x13579BDFU;
    for (size_t i = 0; i < 960; ++i)
    {
        pattern = pattern * 1664525U + 1013904223U;
        input[i] = (int16_t)(((pattern >> 20) & 0x0FFFU) - 2048);
    }

    unsigned char packet[1275];
    const int encoded = opus_encode(encoder, input, 960, packet, sizeof(packet));
    const int decoded = encoded > 0
        ? opus_decode(decoder, packet, encoded, output, 1920, 0) : encoded;
    opus_encoder_destroy(encoder);
    opus_decoder_destroy(decoder);

    printf("Pinned Opus round-trip: encoded=%d decoded=%d\n", encoded, decoded);
    return encoded > 0 && decoded == 960 ? 0 : 4;
}
