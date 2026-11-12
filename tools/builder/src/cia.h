#ifndef TB_CIA_H
#define TB_CIA_H

#include <stddef.h>
#include <stdint.h>

#include "util.h"

typedef struct {
    const uint8_t* smdh;
    const uint8_t* banner;
    const uint8_t* sound;
    size_t sound_len;
} CiaLook;

void ivfc_build(const uint8_t* level3, size_t len, Buf* out);
int cia_build(const uint8_t* tpl, size_t tpl_len, const CiaLook* look, const uint8_t* level3, size_t level3_len, Buf* out);

#endif
