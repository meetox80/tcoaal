#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

namespace {

constexpr uint32_t ALIGN_LOG2 = 2;
constexpr uint32_t ALIGN = 1u << ALIGN_LOG2;
constexpr uint32_t SL_LOG2 = 3;
constexpr uint32_t SL_COUNT = 1u << SL_LOG2;
constexpr uint32_t FL_SHIFT = SL_LOG2 + ALIGN_LOG2;
constexpr uint32_t SMALL = 1u << FL_SHIFT;
constexpr uint32_t FL_COUNT = 22 - FL_SHIFT + 1;
constexpr uint32_t FREE = 1;
constexpr uint32_t PREV_FREE = 2;
constexpr uint32_t FLAGS = FREE | PREV_FREE;
constexpr uint32_t OVERHEAD = 4;
constexpr uint32_t PAYLOAD = 8;
constexpr uint32_t MIN_SIZE = 12;
constexpr uintptr_t RAM_TOP = 0x801FFFF0u;
constexpr uintptr_t STACK_RESERVE = 0x10000;

struct Block {
    Block* prev_phys;
    uint32_t size;
    Block* next_free;
    Block* prev_free;
};

uint32_t g_fl_map;
uint32_t g_sl_map[FL_COUNT];
Block* g_free[FL_COUNT][SL_COUNT];
size_t g_total, g_used, g_peak;

int fls(uint32_t v) { return v ? 31 - __builtin_clz(v) : -1; }
int ffs(uint32_t v) { return v ? __builtin_ctz(v) : -1; }

uint32_t size_of(const Block* b) { return b->size & ~FLAGS; }
void* payload(Block* b) { return reinterpret_cast<uint8_t*>(b) + PAYLOAD; }
Block* block_of(void* p) { return reinterpret_cast<Block*>(static_cast<uint8_t*>(p) - PAYLOAD); }
Block* next_phys(Block* b) { return reinterpret_cast<Block*>(reinterpret_cast<uint8_t*>(b) + OVERHEAD + size_of(b)); }

void mapping(uint32_t size, uint32_t& fl, uint32_t& sl) {
    if (size < SMALL) {
        fl = 0;
        sl = size / (SMALL / SL_COUNT);
        return;
    }
    int top = fls(size);
    sl = (size >> (top - SL_LOG2)) ^ SL_COUNT;
    fl = static_cast<uint32_t>(top) - (FL_SHIFT - 1);
}

void unlink(Block* b, uint32_t fl, uint32_t sl) {
    if (b->next_free) b->next_free->prev_free = b->prev_free;
    if (b->prev_free) b->prev_free->next_free = b->next_free;
    if (g_free[fl][sl] == b) {
        g_free[fl][sl] = b->next_free;
        if (!b->next_free) {
            g_sl_map[fl] &= ~(1u << sl);
            if (!g_sl_map[fl]) g_fl_map &= ~(1u << fl);
        }
    }
}

void remove_free(Block* b) {
    uint32_t fl, sl;
    mapping(size_of(b), fl, sl);
    unlink(b, fl, sl);
}

void insert_free(Block* b) {
    uint32_t fl, sl;
    mapping(size_of(b), fl, sl);
    b->prev_free = nullptr;
    b->next_free = g_free[fl][sl];
    if (b->next_free) b->next_free->prev_free = b;
    g_free[fl][sl] = b;
    g_fl_map |= 1u << fl;
    g_sl_map[fl] |= 1u << sl;
}

void set_free(Block* b, bool free) {
    Block* n = next_phys(b);
    if (free) {
        b->size |= FREE;
        n->size |= PREV_FREE;
        n->prev_phys = b;
    } else {
        b->size &= ~FREE;
        n->size &= ~PREV_FREE;
    }
}

Block* find(uint32_t size) {
    uint32_t fl, sl;
    if (size >= SMALL) size += (1u << (fls(size) - SL_LOG2)) - 1;
    mapping(size, fl, sl);
    if (fl >= FL_COUNT) return nullptr;
    uint32_t sl_map = g_sl_map[fl] & (~0u << sl);
    if (!sl_map) {
        uint32_t fl_map = g_fl_map & (~0u << (fl + 1));
        if (!fl_map) return nullptr;
        fl = static_cast<uint32_t>(ffs(fl_map));
        sl_map = g_sl_map[fl];
    }
    sl = static_cast<uint32_t>(ffs(sl_map));
    Block* b = g_free[fl][sl];
    unlink(b, fl, sl);
    return b;
}

void split(Block* b, uint32_t size) {
    uint32_t total = size_of(b);
    if (total < size + sizeof(Block)) return;
    Block* rest = reinterpret_cast<Block*>(reinterpret_cast<uint8_t*>(b) + OVERHEAD + size);
    rest->size = (total - size - OVERHEAD) | FREE;
    b->size = size | (b->size & FLAGS);
    Block* n = next_phys(rest);
    n->prev_phys = rest;
    n->size |= PREV_FREE;
    insert_free(rest);
}

Block* merge(Block* b) {
    if (b->size & PREV_FREE) {
        Block* p = b->prev_phys;
        remove_free(p);
        p->size += OVERHEAD + size_of(b);
        b = p;
    }
    Block* n = next_phys(b);
    if (n->size & FREE) {
        remove_free(n);
        b->size += OVERHEAD + size_of(n);
    }
    return b;
}

uint32_t adjust(size_t n) {
    if (n > (1u << 21)) return 0;
    uint32_t size = static_cast<uint32_t>((n + ALIGN - 1) & ~(ALIGN - 1));
    return size < MIN_SIZE ? MIN_SIZE : size;
}

}

extern "C" {

void InitHeap(void* addr, size_t size) {
    uintptr_t start = (reinterpret_cast<uintptr_t>(addr) + ALIGN - 1) & ~(uintptr_t)(ALIGN - 1);
    uintptr_t end = reinterpret_cast<uintptr_t>(addr) + size;
    if (end > RAM_TOP - STACK_RESERVE) end = RAM_TOP - STACK_RESERVE;
    end &= ~(uintptr_t)(ALIGN - 1);
    memset(g_free, 0, sizeof(g_free));
    memset(g_sl_map, 0, sizeof(g_sl_map));
    g_fl_map = 0;
    Block* b = reinterpret_cast<Block*>(start);
    uint32_t span = static_cast<uint32_t>(end - start) - OVERHEAD - PAYLOAD;
    b->size = (span & ~(ALIGN - 1)) | FREE;
    Block* sentinel = next_phys(b);
    sentinel->prev_phys = b;
    sentinel->size = PREV_FREE;
    insert_free(b);
    g_total = size_of(b);
    g_used = g_peak = 0;
}

void* malloc(size_t n) {
    uint32_t size = adjust(n);
    Block* b = size ? find(size) : nullptr;
    if (!b) return nullptr;
    split(b, size);
    set_free(b, false);
    g_used += size_of(b) + OVERHEAD;
    if (g_used > g_peak) g_peak = g_used;
    return payload(b);
}

void free(void* p) {
    if (!p) return;
    Block* b = block_of(p);
    g_used -= size_of(b) + OVERHEAD;
    set_free(b, true);
    b = merge(b);
    set_free(b, true);
    insert_free(b);
}

void* calloc(size_t count, size_t size) {
    size_t n = count * size;
    void* p = malloc(n);
    if (p) memset(p, 0, n);
    return p;
}

void* realloc(void* p, size_t n) {
    if (!p) return malloc(n);
    if (!n) {
        free(p);
        return nullptr;
    }
    Block* b = block_of(p);
    uint32_t size = adjust(n), have = size_of(b);
    if (size && size <= have) return p;
    void* q = malloc(n);
    if (q) {
        memcpy(q, p, have);
        free(p);
    }
    return q;
}

void GetHeapUsage(HeapUsage* usage) {
    usage->total = g_total;
    usage->heap = g_total;
    usage->stack = STACK_RESERVE;
    usage->alloc = g_used;
    usage->alloc_max = g_peak;
}

}
