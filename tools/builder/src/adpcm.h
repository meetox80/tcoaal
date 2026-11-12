#ifndef TB_ADPCM_H
#define TB_ADPCM_H

#include <stddef.h>
#include <stdint.h>

#include "util.h"

#define ADPCM_BLOCK_SAMPLES 28
#define ADPCM_BLOCK_BYTES 16
#define STREAM_PAGE 2048

int adpcm_stream_from_ogg(const uint8_t* ogg, size_t len, int rate, int max_channels, int loop, Buf* out);
int adpcm_sample_from_ogg(const uint8_t* ogg, size_t len, int max_rate, Buf* out);
void adpcm_encode(const int16_t* pcm, size_t count, size_t stride, uint8_t* out);

#endif
