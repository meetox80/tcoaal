#pragma once
#include <psxgpu.h>
#include <stdint.h>

#include "../gfx.h"

namespace gfx {

struct Texture {
    RECT area;
    RECT palette;
    RECT fade_palette;
    uint16_t tpage;
    uint16_t clut;
    uint16_t fade_clut;
    uint8_t u, v;
    uint16_t w, h;
    bool semi;
};

}
