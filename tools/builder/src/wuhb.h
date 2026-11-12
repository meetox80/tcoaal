#ifndef TB_WUHB_H
#define TB_WUHB_H

#include <stddef.h>
#include <stdint.h>

#include "image.h"
#include "util.h"

typedef struct {
    const uint8_t* rpx;
    size_t rpx_len;
    Buf icon;
    Buf tv;
    Buf drc;
} Wuhb;

int wuhb_tga_gz(const Image* img, Buf* out);
void wuhb_build(const Wuhb* w, Buf* out);
int wuhb_is_wuhb(const uint8_t* data, size_t len);
int wuhb_replace_rpx(const uint8_t* data, size_t len, const uint8_t* rpx, size_t rpx_len, Buf* out);

#endif
