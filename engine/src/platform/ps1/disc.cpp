#include "disc.h"

#include <psxcd.h>
#include <stdlib.h>
#include <string.h>

namespace disc {

namespace {

constexpr uint32_t VERSION = 3;

enum Async { IDLE, BUSY, DONE, FAILED };

int base;
const Entry* entries;
uint32_t count;
Async g_async = IDLE;

bool issue(uint32_t first, uint32_t n, void* dst) {
    CdlLOC loc;
    CdIntToPos(base + static_cast<int>(first), &loc);
    return CdControl(CdlSetloc, &loc, nullptr) && CdRead(static_cast<int>(n), static_cast<uint32_t*>(dst), CdlModeSpeed);
}

bool read_sectors(uint32_t first, uint32_t n, void* dst) {
    settle();
    return issue(first, n, dst) && CdReadSync(0, nullptr) == 0;
}

uint32_t rd32(const uint8_t* p) { return p[0] | p[1] << 8 | p[2] << 16 | static_cast<uint32_t>(p[3]) << 24; }

}

uint32_t hash(const char* name) {
    uint32_t h = 2166136261u;
    for (auto* p = reinterpret_cast<const uint8_t*>(name); *p; p++) h = (h ^ *p) * 16777619u;
    return h;
}

bool init() {
    CdInit();
    CdlFILE file;
    if (!CdSearchFile(&file, "\\TCOAAL.PAK;1")) return false;
    base = CdPosToInt(&file.pos);
    alignas(4) static uint8_t first[SECTOR];
    if (!read_sectors(0, 1, first) || memcmp(first, "TPAK", 4) != 0 || rd32(first + 4) != VERSION) return false;
    count = rd32(first + 8);
    uint32_t n = sectors(16 + count * sizeof(Entry));
    auto* index = static_cast<uint8_t*>(malloc(n * SECTOR));
    if (!index || !read_sectors(0, n, index)) {
        free(index);
        return false;
    }
    entries = reinterpret_cast<const Entry*>(index + 16);
    return true;
}

const Entry* find(const char* name) {
    uint32_t h = hash(name), lo = 0, hi = count;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        if (entries[mid].hash < h) lo = mid + 1;
        else hi = mid;
    }
    return lo < count && entries[lo].hash == h ? &entries[lo] : nullptr;
}

bool read(const Entry& e, void* dst) { return read_sectors(e.sector, sectors(e.size), dst); }

bool read_part(const Entry& e, uint32_t first, uint32_t n, void* dst) { return read_sectors(e.sector + first, n, dst); }

void settle() {
    if (g_async == BUSY) g_async = CdReadSync(0, nullptr) == 0 ? DONE : FAILED;
}

bool start_read(const Entry& e, uint32_t first, uint32_t n, void* dst) {
    settle();
    g_async = issue(e.sector + first, n, dst) ? BUSY : FAILED;
    return g_async == BUSY;
}

int poll() {
    if (g_async == BUSY) {
        int left = CdReadSync(1, nullptr);
        if (left > 0) return 1;
        g_async = left == 0 ? DONE : FAILED;
    }
    Async state = g_async;
    if (state == DONE || state == FAILED) g_async = IDLE;
    return state == DONE ? 0 : state == FAILED ? -1 : 2;
}

}
