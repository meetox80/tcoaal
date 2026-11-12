#ifndef TB_VPK_H
#define TB_VPK_H

#include <stddef.h>
#include <stdint.h>

#include "util.h"

typedef struct {
    const uint8_t* eboot;
    size_t eboot_len;
    const uint8_t* sfo;
    size_t sfo_len;
    Buf icon0;
    Buf bg;
    Buf startup;
} Vpk;

int vpk_build(const Vpk* v, Buf* out);
int vpk_is_vpk(const uint8_t* data, size_t len);
int vpk_replace_eboot(const uint8_t* vpk, size_t len, const uint8_t* eboot, size_t eboot_len, Buf* out);

#endif
