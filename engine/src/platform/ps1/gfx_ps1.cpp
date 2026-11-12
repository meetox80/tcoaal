#include "../gfx.h"

#include <psxgpu.h>
#include <stdlib.h>

#include "texture.h"
#include "video.h"
#include "vram.h"

namespace gfx {

namespace {

struct Pass {
    int16_t r, g, b;
    uint8_t abr;
    bool semi;
};

int plan(uint32_t color, Blend blend, bool layered, Pass* out) {
    int r = color & 255, g = color >> 8 & 255, b = color >> 16 & 255, a = color >> 24;
    switch (blend) {
        case Blend::Add:
        case Blend::Screen:
            if (!(r | g | b)) return 0;
            out[0] = {static_cast<int16_t>(r), static_cast<int16_t>(g), static_cast<int16_t>(b), 1, true};
            return 1;
        case Blend::Subtract:
            if (!(r | g | b)) return 0;
            out[0] = {static_cast<int16_t>(r), static_cast<int16_t>(g), static_cast<int16_t>(b), 2, true};
            return 1;
        default: break;
    }
    if (a >= 224 || blend == Blend::Copy) {
        out[0] = {static_cast<int16_t>(r), static_cast<int16_t>(g), static_cast<int16_t>(b), 0, false};
        return 1;
    }
    if (a < 16) return 0;
    if (a < 64) {
        out[0] = {static_cast<int16_t>(r), static_cast<int16_t>(g), static_cast<int16_t>(b), 1, true};
        return 1;
    }
    if (a < 160 || layered) {
        out[0] = {static_cast<int16_t>(r * 2), static_cast<int16_t>(g * 2), static_cast<int16_t>(b * 2), 0, true};
        return 1;
    }
    out[0] = {static_cast<int16_t>(r * 4 / 3), static_cast<int16_t>(g * 4 / 3), static_cast<int16_t>(b * 4 / 3), 0, true};
    out[1] = out[0];
    return 2;
}

uint8_t tex_level(int w) {
    int v = (w * 128 + 127) / 255;
    return static_cast<uint8_t>(v > 255 ? 255 : v);
}

uint8_t flat_level(int w) { return static_cast<uint8_t>(w > 255 ? 255 : w); }

int round_i(float v) { return static_cast<int>(v < 0 ? v - 0.5f : v + 0.5f); }

bool offscreen(const int* xy) {
    int x0 = xy[0], x1 = xy[0], y0 = xy[1], y1 = xy[1];
    for (int k = 2; k < 8; k += 2) {
        if (xy[k] < x0) x0 = xy[k];
        if (xy[k] > x1) x1 = xy[k];
        if (xy[k + 1] < y0) y0 = xy[k + 1];
        if (xy[k + 1] > y1) y1 = xy[k + 1];
    }
    return x1 <= 0 || y1 <= 0 || x0 >= SCREEN_W || y0 >= SCREEN_H || x1 - x0 > 1023 || y1 - y0 > 511;
}

void textured(const Texture* t, const int* xy, const int* uv, uint32_t color, Blend blend) {
    if (offscreen(xy)) return;
    Pass passes[2];
    int n = plan(color, blend, t->semi, passes);
    for (int i = 0; i < n; i++) {
        const Pass& p = passes[i];
        POLY_FT4* q = video::push<POLY_FT4>();
        setPolyFT4(q);
        setRGB0(q, tex_level(p.r), tex_level(p.g), tex_level(p.b));
        setSemiTrans(q, p.semi || t->semi);
        setXY4(q, xy[0], xy[1], xy[2], xy[3], xy[4], xy[5], xy[6], xy[7]);
        setUV4(q, uv[0], uv[1], uv[2], uv[3], uv[4], uv[5], uv[6], uv[7]);
        q->tpage = static_cast<uint16_t>(t->tpage | (p.abr << 5));
        q->clut = p.semi ? t->fade_clut : t->clut;
    }
}

void tile(int x0, int y0, int x1, int y1, const Pass& p) {
    if (p.semi) video::mode(getTPage(0, p.abr, 0, 0));
    TILE* t = video::push<TILE>();
    setTile(t);
    setRGB0(t, flat_level(p.r), flat_level(p.g), flat_level(p.b));
    setSemiTrans(t, p.semi);
    setXY0(t, x0, y0);
    setWH(t, x1 - x0, y1 - y0);
}

}

void reserve(size_t) {}
void* alloc(size_t bytes) { return malloc(bytes); }
void release(void* p) { free(p); }

void texture_destroy(Texture* t) {
    if (!t) return;
    vram::release(t->area);
    if (t->palette.w) vram::release_clut(t->palette);
    if (t->fade_palette.w) vram::release_clut(t->fade_palette);
    delete t;
}

size_t texture_bytes(const Texture* t) { return t ? static_cast<size_t>(t->area.w) * t->area.h * 2 : 0; }
int texture_width(const Texture* t) { return t ? t->w : 0; }
int texture_height(const Texture* t) { return t ? t->h : 0; }

void init() {}
void shutdown() {}
void begin_frame(uint32_t) {}
void end_frame() {}
const uint32_t* framebuffer() { return nullptr; }

void set_scissor(int x, int y, int w, int h) {
    int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
    int x1 = x + w > SCREEN_W ? SCREEN_W : x + w, y1 = y + h > SCREEN_H ? SCREEN_H : y + h;
    if (x1 <= x0 || y1 <= y0) x1 = x0 + 1, y1 = y0 + 1;
    video::clip(x0, y0, x1, y1);
}

void reset_scissor() { video::clip(0, 0, SCREEN_W, SCREEN_H); }

void sprite_px(Texture* t, int u0, int v0, int u1, int v1, int x0, int y0, int x1, int y1, uint32_t color, Blend blend) {
    if (!t || x0 == x1 || y0 == y1) return;
    u0 += t->u, v0 += t->v, u1 += t->u, v1 += t->v;
    int xy[8] = {x0, y0, x1, y0, x0, y1, x1, y1};
    int uv[8] = {u0, v0, u1, v0, u0, v1, u1, v1};
    textured(t, xy, uv, color, blend);
}

void sprite(Texture* t, float u0, float v0, float u1, float v1, float x0, float y0, float x1, float y1, uint32_t color, Blend blend, bool) {
    sprite_px(t, round_i(u0), round_i(v0), round_i(u1), round_i(v1), round_i(x0), round_i(y0), round_i(x1), round_i(y1), color, blend);
}

void draw_quad(Texture* t, const float uv[8], const float xy[8], uint32_t color, Blend blend, bool) {
    if (!t) return;
    static const int ORDER[4] = {0, 1, 3, 2};
    int pxy[8], puv[8];
    for (int k = 0; k < 4; k++) {
        int s = ORDER[k] * 2;
        pxy[k * 2] = round_i(xy[s]);
        pxy[k * 2 + 1] = round_i(xy[s + 1]);
        puv[k * 2] = t->u + round_i(uv[s]);
        puv[k * 2 + 1] = t->v + round_i(uv[s + 1]);
    }
    textured(t, pxy, puv, color, blend);
}

void fill(float fx0, float fy0, float fx1, float fy1, uint32_t color, Blend blend) {
    int x0 = round_i(fx0), y0 = round_i(fy0), x1 = round_i(fx1), y1 = round_i(fy1);
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > SCREEN_W) x1 = SCREEN_W;
    if (y1 > SCREEN_H) y1 = SCREEN_H;
    if (x1 <= x0 || y1 <= y0) return;
    int a = color >> 24;
    bool full = x0 == 0 && y0 == 0 && x1 == SCREEN_W && y1 == SCREEN_H;
    if (full && blend == Blend::Normal && a < 224) {
        video::dim((255 - a) * 256 / 255);
        Pass add = {static_cast<int16_t>(color & 255), static_cast<int16_t>(color >> 8 & 255), static_cast<int16_t>(color >> 16 & 255), 1, true};
        if (add.r | add.g | add.b) tile(x0, y0, x1, y1, add);
        return;
    }
    Pass passes[2];
    int n = plan(color, blend, false, passes);
    for (int i = 0; i < n; i++) tile(x0, y0, x1, y1, passes[i]);
}

void desaturate(float) {}

}
