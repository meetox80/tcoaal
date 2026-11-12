#pragma once
#include <psxgpu.h>

namespace vram {

void reset();
bool alloc(int words, int rows, int texels_per_word, RECT& out);
void release(const RECT& r);
bool alloc_clut(int entries, RECT& out);
void release_clut(const RECT& r);
void upload(const RECT& r, const void* data);

}
