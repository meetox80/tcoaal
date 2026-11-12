#pragma once
#include <stdint.h>

namespace timebase {

constexpr uint32_t PER_SECOND = 33868800 / 8;

void init();
uint64_t now();
void hold();
void release();

}
