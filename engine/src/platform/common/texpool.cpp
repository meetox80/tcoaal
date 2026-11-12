#include "texpool.h"

#include <algorithm>
#include <cstdint>
#include <vector>

#include "../platform.h"

namespace texpool {

namespace {

constexpr size_t HEAD = 64;

struct Span {
    size_t at, size;
};

uint8_t* g_base = nullptr;
size_t g_size = 0;
std::vector<Span> g_free;
plat::Mutex* g_lock = nullptr;

size_t round_up(size_t n) { return (n + HEAD - 1) & ~(HEAD - 1); }

struct Lock {
    Lock() { plat::mutex_lock(g_lock); }
    ~Lock() { plat::mutex_unlock(g_lock); }
};

}

void init(void* base, size_t bytes) {
    uintptr_t start = ((uintptr_t)base + HEAD - 1) & ~(uintptr_t)(HEAD - 1);
    size_t skip = start - (uintptr_t)base;
    if (!base || bytes <= skip + HEAD * 2) return;
    g_lock = plat::mutex_create();
    g_base = (uint8_t*)start;
    g_size = (bytes - skip) & ~(HEAD - 1);
    g_free.reserve(512);
    g_free.push_back(Span{0, g_size});
}

void* alloc(size_t bytes) {
    if (!g_base) return nullptr;
    size_t need = round_up(bytes ? bytes : 1) + HEAD;
    Lock lock;
    auto best = g_free.end();
    for (auto it = g_free.begin(); it != g_free.end(); ++it) {
        if (it->size >= need && (best == g_free.end() || it->size < best->size)) best = it;
    }
    if (best == g_free.end()) return nullptr;
    size_t at = best->at;
    best->at += need;
    best->size -= need;
    if (best->size == 0) g_free.erase(best);
    *(size_t*)(g_base + at) = need;
    return g_base + at + HEAD;
}

bool release(void* p) {
    uint8_t* q = (uint8_t*)p;
    if (!g_base || q < g_base + HEAD || q >= g_base + g_size) return false;
    size_t at = (size_t)(q - g_base) - HEAD;
    size_t size = *(size_t*)(g_base + at);
    Lock lock;
    auto next = std::lower_bound(g_free.begin(), g_free.end(), at, [](const Span& s, size_t v) { return s.at < v; });
    bool join_prev = next != g_free.begin() && (next - 1)->at + (next - 1)->size == at;
    bool join_next = next != g_free.end() && at + size == next->at;
    if (join_prev && join_next) {
        (next - 1)->size += size + next->size;
        g_free.erase(next);
    } else if (join_prev) {
        (next - 1)->size += size;
    } else if (join_next) {
        next->at = at;
        next->size += size;
    } else {
        g_free.insert(next, Span{at, size});
    }
    return true;
}

size_t capacity() { return g_size; }

size_t largest_free() {
    if (!g_base) return 0;
    Lock lock;
    size_t best = 0;
    for (const Span& s : g_free) best = std::max(best, s.size);
    return best > HEAD ? best - HEAD : 0;
}

}
