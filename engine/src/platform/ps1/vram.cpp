#include "vram.h"

#include <hwregs_c.h>
#include <psxetc.h>
#include <stdint.h>

#include "video.h"

namespace vram {

namespace {

constexpr int CELL = 16;
constexpr int COLS = 1024 / CELL;
constexpr int ROWS = 512 / CELL;
constexpr int REACH = 255;
constexpr int FB_COLS = video::W / CELL;
constexpr int FB_ROWS = (video::H + CELL - 1) / CELL;
constexpr int CLUT_Y = 480;
constexpr int CLUT_ROWS = 512 - CLUT_Y;
constexpr int BIG_PER_ROW = 2;
constexpr int SMALL_X = BIG_PER_ROW * 256;
constexpr int SMALL_PER_ROW = (video::W - SMALL_X) / 16;

uint64_t used[ROWS];
uint8_t big_used[CLUT_ROWS * BIG_PER_ROW];
uint8_t small_used[CLUT_ROWS * SMALL_PER_ROW];

uint64_t span(int x, int n) { return (n >= 64 ? ~0ULL : ((1ULL << n) - 1)) << x; }

void mark(int x, int y, int w, int h, bool on) {
    uint64_t m = span(x, w);
    for (int r = y; r < y + h; r++) used[r] = on ? used[r] | m : used[r] & ~m;
}

bool free_at(int x, int y, int w, int h) {
    uint64_t m = span(x, w);
    for (int r = y; r < y + h; r++) {
        if (used[r] & m) return false;
    }
    return true;
}

}

void reset() {
    for (uint64_t& r : used) r = 0;
    mark(0, 0, FB_COLS, FB_ROWS, true);
    mark(0, CLUT_Y / CELL, FB_COLS, CLUT_ROWS / CELL, true);
    for (uint8_t& b : big_used) b = 0;
    for (uint8_t& b : small_used) b = 0;
}

bool alloc(int words, int rows, int per, RECT& out) {
    if (words <= 0 || rows <= 0 || words * per > REACH || rows > REACH) return false;
    int w = (words + CELL - 1) / CELL, h = (rows + CELL - 1) / CELL;
    for (int y = 0; y + h <= ROWS; y++) {
        int band_row = (y * CELL) & 255;
        if (band_row + rows > REACH || (y * CELL) / 256 != (y * CELL + rows - 1) / 256) continue;
        for (int x = 0; x + w <= COLS; x++) {
            if ((((x * CELL) & 63) + words) * per > REACH || !free_at(x, y, w, h)) continue;
            mark(x, y, w, h, true);
            out = {static_cast<int16_t>(x * CELL), static_cast<int16_t>(y * CELL), static_cast<int16_t>(words), static_cast<int16_t>(rows)};
            return true;
        }
    }
    return false;
}

void release(const RECT& r) { mark(r.x / CELL, r.y / CELL, (r.w + CELL - 1) / CELL, (r.h + CELL - 1) / CELL, false); }

bool alloc_clut(int entries, RECT& out) {
    bool big = entries > 16;
    uint8_t* slots = big ? big_used : small_used;
    int per_row = big ? BIG_PER_ROW : SMALL_PER_ROW, count = CLUT_ROWS * per_row;
    for (int i = 0; i < count; i++) {
        if (slots[i]) continue;
        slots[i] = 1;
        int x = big ? (i % per_row) * 256 : SMALL_X + (i % per_row) * 16;
        out = {static_cast<int16_t>(x), static_cast<int16_t>(CLUT_Y + i / per_row), static_cast<int16_t>(entries), 1};
        return true;
    }
    return false;
}

void upload(const RECT& r, const void* data) {
    DrawSync(0);
    const uint32_t* src = static_cast<const uint32_t*>(data);
    uint32_t words = (static_cast<uint32_t>(r.w) * static_cast<uint32_t>(r.h) + 1) / 2;
    while (!(GPU_GP1 & (1 << 26))) {
    }
    GPU_GP1 = 0x04000000;
    GPU_GP0 = 0x01000000;
    GPU_GP0 = 0xA0000000;
    GPU_GP0 = static_cast<uint16_t>(r.x) | static_cast<uint32_t>(static_cast<uint16_t>(r.y)) << 16;
    GPU_GP0 = static_cast<uint16_t>(r.w) | static_cast<uint32_t>(static_cast<uint16_t>(r.h)) << 16;
    uint32_t bulk = words & ~15u;
    if (bulk) {
        GPU_GP1 = 0x04000002;
        while (DMA_CHCR(DMA_GPU) & (1 << 24)) {
        }
        DMA_MADR(DMA_GPU) = reinterpret_cast<uint32_t>(src);
        DMA_BCR(DMA_GPU) = 16 | (bulk / 16) << 16;
        DMA_CHCR(DMA_GPU) = 0x01000201;
        while (DMA_CHCR(DMA_GPU) & (1 << 24)) {
        }
        GPU_GP1 = 0x04000000;
    }
    for (uint32_t i = bulk; i < words; i++) {
        while (!(GPU_GP1 & (1 << 28))) {
        }
        GPU_GP0 = src[i];
    }
}

void release_clut(const RECT& r) {
    int row = r.y - CLUT_Y;
    if (r.w > 16) big_used[row * BIG_PER_ROW + r.x / 256] = 0;
    else small_used[row * SMALL_PER_ROW + (r.x - SMALL_X) / 16] = 0;
}

}
