#ifndef ALAC_BRIDGE_H
#define ALAC_BRIDGE_H

#ifdef __cplusplus
extern "C" {
#endif

/* Returns a decoder for AirPlay's ALAC stream, or NULL. */
void *alac_decoder_create(int frame_length, int sample_rate);

/* Decodes one ALAC frame into 16-bit interleaved PCM; returns the number of bytes written. */
int alac_decoder_decode(void *handle, const unsigned char *in, int in_len,
                        unsigned char *out, int out_capacity);

void alac_decoder_destroy(void *handle);

#ifdef __cplusplus
}
#endif
#endif
