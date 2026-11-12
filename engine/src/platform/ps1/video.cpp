#include "video.h"

#include <psxgpu.h>

namespace video {

namespace {

constexpr size_t PACKET_BYTES = 49152;

DISPENV disp;
DRAWENV draw;
uint32_t head[2];
alignas(4) uint8_t packets[2][PACKET_BYTES];
alignas(4) uint8_t spill[64];
uint8_t* cursor;
uint32_t* tail;
int db;

void open(int buffer) {
    db = buffer;
    cursor = packets[db];
    head[db] = 0x00FFFFFF;
    tail = &head[db];
    clip(0, 0, W, H);
    mode(getTPage(0, 0, 0, 0));
    TILE* clear = push<TILE>();
    setTile(clear);
    setRGB0(clear, 0, 0, 0);
    setXY0(clear, 0, 0);
    setWH(clear, W, H);
}

uint32_t* next_of(uint32_t* p) { return reinterpret_cast<uint32_t*>(0x80000000u | (*p & 0x00FFFFFFu)); }

}

void init() {
    ResetGraph(0);
    SetDefDispEnv(&disp, 0, 0, W, H);
    disp.isinter = 1;
    disp.screen.h = H / 2;
    SetDefDrawEnv(&draw, 0, 0, W, H);
    draw.dfe = 0;
    draw.isbg = 0;
    PutDispEnv(&disp);
    PutDrawEnv(&draw);
    open(0);
}

bool pal() { return GetVideoMode() == MODE_PAL; }

int rate() { return pal() ? 50 : 60; }

uint32_t fields() { return static_cast<uint32_t>(VSync(-1)); }

uint8_t* reserve(size_t bytes) {
    if (bytes > sizeof(spill) || cursor + bytes > packets[db] + PACKET_BYTES) return spill;
    uint8_t* p = cursor;
    cursor += bytes;
    catPrim(tail, p);
    tail = reinterpret_cast<uint32_t*>(p);
    return p;
}

void mode(uint16_t tpage) {
    DR_TPAGE* t = push<DR_TPAGE>();
    setDrawTPage(t, 0, 1, tpage);
}

void clip(int x0, int y0, int x1, int y1) {
    DR_AREA* a = push<DR_AREA>();
    setDrawAreaXY(a, x0, y0, x1 - 1, y1 - 1);
}

void dim(int keep) {
    if (keep >= 256) return;
    if (keep < 0) keep = 0;
    termPrim(tail);
    for (uint32_t* p = next_of(&head[db]);; p = next_of(p)) {
        uint32_t len = *p >> 24;
        if (len) {
            uint8_t* c = reinterpret_cast<uint8_t*>(p + 1);
            if (c[3] < 0xE0) {
                c[0] = static_cast<uint8_t>(c[0] * keep >> 8);
                c[1] = static_cast<uint8_t>(c[1] * keep >> 8);
                c[2] = static_cast<uint8_t>(c[2] * keep >> 8);
            }
        }
        if (p == tail) break;
    }
}

void flip() {
    termPrim(tail);
    DrawSync(0);
    VSync(0);
    DrawOTag(&head[db]);
    SetDispMask(1);
    open(db ^ 1);
}

}
