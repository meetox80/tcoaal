#ifndef TB_PS2ICON_H
#define TB_PS2ICON_H

#include "image.h"
#include "util.h"

void ps2_icon_sys(const char* title, Buf* out);
void ps2_icon_icn(const Image* art, Buf* out);

#endif
