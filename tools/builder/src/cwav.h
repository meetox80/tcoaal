#ifndef TB_CWAV_H
#define TB_CWAV_H

#include <stddef.h>
#include <stdint.h>

#include "util.h"

void cwav_build(const int16_t* pcm, size_t frames, int channels, int rate, Buf* out);
int cwav_from_ogg(const uint8_t* ogg, size_t len, double seconds, Buf* out);

#endif
