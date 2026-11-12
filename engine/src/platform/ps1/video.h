#pragma once
#include <stddef.h>
#include <stdint.h>

namespace video {

constexpr int W = 640;
constexpr int H = 360;

void init();
bool pal();
int rate();
uint32_t fields();
uint8_t* reserve(size_t bytes);
void mode(uint16_t tpage);
void clip(int x0, int y0, int x1, int y1);
void dim(int keep);
void flip();

template <class T>
inline T* push() {
    return reinterpret_cast<T*>(reserve(sizeof(T)));
}

}
