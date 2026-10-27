/*
 * C wrapper around Apple's ALAC decoder (Apache-2.0) for AirPlay audio-only streams,
 * which arrive as ALAC (ct=2): 44.1 kHz, 16-bit, stereo, 352 samples per frame.
 */

#include <stdint.h>
#include <string.h>
#include <new>

#include "ALACDecoder.h"
#include "ALACBitUtilities.h"
#include "alac_bridge.h"

static void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t) (v >> 24);
    p[1] = (uint8_t) (v >> 16);
    p[2] = (uint8_t) (v >> 8);
    p[3] = (uint8_t) v;
}

extern "C" void *alac_decoder_create(int frame_length, int sample_rate)
{
    /* ALACSpecificConfig ("magic cookie"), big-endian, with the parameters AirPlay uses */
    uint8_t cookie[24] = {0};
    put_u32(cookie, (uint32_t) frame_length);
    cookie[4] = 0;      /* compatibleVersion */
    cookie[5] = 16;     /* bitDepth */
    cookie[6] = 40;     /* pb */
    cookie[7] = 10;     /* mb */
    cookie[8] = 14;     /* kb */
    cookie[9] = 2;      /* numChannels */
    cookie[10] = 0;     /* maxRun = 255 */
    cookie[11] = 255;
    put_u32(cookie + 12, 0);    /* maxFrameBytes */
    put_u32(cookie + 16, 0);    /* avgBitRate */
    put_u32(cookie + 20, (uint32_t) sample_rate);

    ALACDecoder *decoder = new (std::nothrow) ALACDecoder();
    if (!decoder) {
        return nullptr;
    }
    if (decoder->Init(cookie, sizeof(cookie)) != 0) {
        delete decoder;
        return nullptr;
    }
    return decoder;
}

extern "C" int alac_decoder_decode(void *handle, const unsigned char *in, int in_len,
                                   unsigned char *out, int out_capacity)
{
    ALACDecoder *decoder = static_cast<ALACDecoder *>(handle);
    if (!decoder || in_len <= 0) {
        return 0;
    }
    uint32_t frame_length = decoder->mConfig.frameLength;
    uint32_t channels = decoder->mConfig.numChannels;
    if ((int) (frame_length * channels * 2) > out_capacity) {
        return 0;
    }

    BitBuffer bits;
    BitBufferInit(&bits, const_cast<uint8_t *>(in), (uint32_t) in_len);
    uint32_t samples = 0;
    if (decoder->Decode(&bits, out, frame_length, channels, &samples) != 0) {
        return 0;
    }
    return (int) (samples * channels * 2);
}

extern "C" void alac_decoder_destroy(void *handle)
{
    delete static_cast<ALACDecoder *>(handle);
}
