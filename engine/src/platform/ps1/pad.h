#pragma once
#include <stdint.h>

namespace pad {

void init();
void poll();
uint16_t held();
uint16_t pressed();

}
