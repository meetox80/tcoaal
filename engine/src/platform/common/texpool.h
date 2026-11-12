#pragma once
#include <cstddef>

namespace texpool {

void init(void* base, size_t bytes);
void* alloc(size_t bytes);
bool release(void* p);
size_t capacity();
size_t largest_free();

}
