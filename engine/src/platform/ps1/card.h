#pragma once
#include <stddef.h>
#include <stdint.h>

#include <string>
#include <vector>

namespace card {

void init();
bool load(char slot, std::vector<uint8_t>& payload);
bool store(char slot, const char* title, const std::vector<uint8_t>& payload);
bool erase(char slot);
bool present(char slot);

}
