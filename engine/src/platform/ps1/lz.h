#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

namespace lz {

std::vector<uint8_t> compress(const uint8_t* src, size_t n);
bool decompress(const uint8_t* src, size_t n, uint8_t* dst, size_t out_n);
uint32_t checksum(const uint8_t* p, size_t n);

}
