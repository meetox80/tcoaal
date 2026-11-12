#pragma once
#include <stdint.h>

namespace disc {

constexpr uint32_t SECTOR = 2048;

struct Entry {
    uint32_t hash;
    uint32_t sector;
    uint32_t size;
};

inline uint32_t sectors(uint32_t bytes) { return (bytes + SECTOR - 1) / SECTOR; }

uint32_t hash(const char* name);
bool init();
const Entry* find(const char* name);
bool read(const Entry& e, void* dst);
bool read_part(const Entry& e, uint32_t first, uint32_t count, void* dst);
bool start_read(const Entry& e, uint32_t first, uint32_t count, void* dst);
int poll();
void settle();

}
