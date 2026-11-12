#ifndef TB_SND0_H
#define TB_SND0_H

#include <stddef.h>
#include <stdint.h>

#include "util.h"

int snd0_from_ogg(const uint8_t* ogg, size_t len, Buf* out);

#endif
