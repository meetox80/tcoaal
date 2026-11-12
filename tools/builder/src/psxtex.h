#ifndef TB_PSXTEX_H
#define TB_PSXTEX_H

#include <stddef.h>
#include <stdint.h>

#include "image.h"
#include "util.h"

#define PSX_DIV 4
#define PSX_CHUNK 252

typedef enum { PSX_4BIT = 0, PSX_8BIT = 1, PSX_15BIT = 2 } PsxDepth;

int psx_scaled(int v);
Image* psx_resize(const Image* straight);
Image* psx_resize_cells(const Image* straight, int cols, int rows);
int psx_tex_encode(const Image* straight, int orig_w, int orig_h, PsxDepth depth, Buf* out);
int psx_card_icon(const uint8_t* png, size_t len, Buf* out);
int psx_tex_from_png(const uint8_t* png, size_t len, int cols, int rows, PsxDepth depth, Buf* out);

#endif
