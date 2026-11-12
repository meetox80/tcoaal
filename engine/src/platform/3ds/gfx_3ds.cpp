#include "../gfx.h"
#include "../platform.h"

#include <3ds.h>
#include <citro3d.h>
#include <malloc.h>

#include <algorithm>
#include <cstring>
#include <vector>

#include "sprite_shbin.h"

namespace gfx {

struct Texture {
    int w = 0;
    int h = 0;
    float iw = 0;
    float ih = 0;
    size_t bytes = 0;
    C3D_Tex tex{};
};

struct Vtx {
    float x, y;
    float u, v;
    uint32_t color;
};

enum class Mode : uint8_t { Texture, Fill, Gray };

constexpr int CANVAS = 512;
constexpr int PANEL_CANVAS_W = 512;
constexpr int PANEL_CANVAS_H = 256;
constexpr int TOP_W = 400;
constexpr int TOP_H = 240;
constexpr int MAX_QUADS = 16384;
constexpr int BATCH_QUADS = 4096;
constexpr uint32_t TRANSFER_FLAGS = GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) |
                                    GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) | GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB8) |
                                    GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO);

static DVLB_s* g_dvlb = nullptr;
static shaderProgram_s g_program;
static int g_proj_loc = -1;
static C3D_Mtx g_proj_canvas;
static C3D_Mtx g_proj_top;
static C3D_Mtx g_proj_panel;
static C3D_Mtx g_proj_bottom;
static C3D_RenderTarget* g_top = nullptr;
static C3D_RenderTarget* g_bottom = nullptr;
static Texture g_canvas[2];
static C3D_RenderTarget* g_canvas_target[2] = {};
static int g_cur = 0;
static Texture g_panel;
static C3D_RenderTarget* g_panel_target = nullptr;
static bool g_in_panel = false;
static bool g_panel_drawn = false;
static int g_target_h = CANVAS;
static int g_clip_w = SCREEN_W;
static int g_clip_h = SCREEN_H;

static Vtx* g_vbuf = nullptr;
static uint16_t* g_ibuf = nullptr;
static int g_used = 0;
static int g_start = 0;
static int g_limit = MAX_QUADS;
static bool g_overflow = false;

static Mode g_mode = Mode::Texture;
static Texture* g_tex = nullptr;
static Blend g_blend = Blend::Normal;
static bool g_smooth = false;
static int g_gray = 0;

static bool g_state_valid = false;
static Mode g_bound_mode = Mode::Fill;
static Texture* g_bound_tex = nullptr;
static bool g_bound_smooth = false;
static Blend g_bound_blend = Blend::Copy;
static int g_bound_gray = -1;

static int g_sx0 = 0, g_sy0 = 0, g_sx1 = SCREEN_W, g_sy1 = SCREEN_H;
static bool g_scissor_empty = false;

static std::vector<Texture*> g_garbage;
static std::vector<uint32_t> g_shot;
static std::vector<uint32_t> g_panel_shot;
static uint8_t g_morton[64];

void reserve(size_t) {}
void* alloc(size_t bytes) { return memalign(64, bytes ? bytes : 16); }
void release(void* p) { free(p); }

static inline float tc(const Texture* t, float v) { return 1.0f - v * t->ih; }

static int pow2(int n) {
    int p = 8;
    while (p < n) p <<= 1;
    return p;
}

static int bpp_of(Format f) {
    switch (f) {
        case Format::RGBA8888: return 4;
        case Format::T8: return 1;
        default: return 2;
    }
}

static uint32_t expand(Format f, const uint8_t* p, const uint32_t* pal) {
    switch (f) {
        case Format::RGBA8888: return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
        case Format::ARGB8888: return (uint32_t)p[1] | ((uint32_t)p[2] << 8) | ((uint32_t)p[3] << 16) | ((uint32_t)p[0] << 24);
        case Format::T8: return pal ? pal[p[0]] : 0;
        case Format::RGBA4444: {
            uint16_t v = (uint16_t)(p[0] | (p[1] << 8));
            return rgba((v & 15) * 17, ((v >> 4) & 15) * 17, ((v >> 8) & 15) * 17, ((v >> 12) & 15) * 17);
        }
        case Format::RGB565: {
            uint16_t v = (uint16_t)(p[0] | (p[1] << 8));
            return rgba((v & 31) * 255 / 31, ((v >> 5) & 63) * 255 / 63, ((v >> 11) & 31) * 255 / 31, 255);
        }
        case Format::RGBA5551: {
            uint16_t v = (uint16_t)(p[0] | (p[1] << 8));
            return rgba((v & 31) * 255 / 31, ((v >> 5) & 31) * 255 / 31, ((v >> 10) & 31) * 255 / 31, (v >> 15) ? 255 : 0);
        }
    }
    return 0;
}

static uint32_t fetch(const uint8_t* src, int x, int y, int stride, Format fmt, bool tiled, const uint32_t* pal) {
    if (tiled) {
        const uint8_t* p = src + (((size_t)(y / 8) * (stride / 8) + x / 8) * 64 + g_morton[(y % 8) * 8 + x % 8]) * 4;
        return rgba(p[3], p[2], p[1], p[0]);
    }
    int bpp = bpp_of(fmt);
    return expand(fmt, src + ((size_t)y * stride + x) * bpp, pal);
}

Texture* texture_create(int w, int h, int stride, int rows, Format fmt, bool tiled, void* pixels, uint32_t* palette) {
    Texture* t = new Texture();
    t->w = w;
    t->h = h;
    int tw = pow2(w), th = pow2(h);
    int alloc_rows = std::min(th, (h + 7) / 8 * 8 + 8);
    bool direct = fmt == Format::RGBA8888 && tiled && stride == tw && rows % 8 == 0 && rows <= th && rows >= std::min(th, (h + 7) / 8 * 8);
    if (direct) alloc_rows = rows;
    size_t size = (size_t)tw * alloc_rows * 4;
    uint32_t* dst = w > 0 && h > 0 ? (uint32_t*)linearAlloc(size) : nullptr;
    if (!dst) {
        plat::log("gfx: no linear memory for %dx%d texture", w, h);
        release(pixels);
        release(palette);
        return t;
    }
    const uint8_t* src = (const uint8_t*)pixels;
    if (direct) {
        std::memcpy(dst, src, size);
    } else {
        for (int ty = 0; ty < alloc_rows / 8; ty++) {
            for (int tx = 0; tx < tw / 8; tx++) {
                uint32_t* out = dst + ((size_t)ty * (tw / 8) + tx) * 64;
                for (int r = 0; r < 8; r++) {
                    int y = std::min(ty * 8 + r, h - 1);
                    for (int c = 0; c < 8; c++) {
                        int x = std::min(tx * 8 + c, w - 1);
                        out[g_morton[r * 8 + c]] = __builtin_bswap32(fetch(src, x, y, stride, fmt, tiled, palette));
                    }
                }
            }
        }
    }
    GSPGPU_FlushDataCache(dst, size);
    release(pixels);
    release(palette);
    t->tex.data = dst;
    t->tex.fmt = GPU_RGBA8;
    t->tex.size = size;
    t->tex.width = (u16)tw;
    t->tex.height = (u16)th;
    t->tex.param = GPU_TEXTURE_MODE(GPU_TEX_2D);
    C3D_TexSetWrap(&t->tex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
    t->iw = 1.0f / tw;
    t->ih = 1.0f / th;
    t->bytes = size;
    return t;
}

void texture_destroy(Texture* t) {
    if (!t) return;
    g_garbage.push_back(t);
}

static void collect_garbage() {
    for (Texture* t : g_garbage) {
        if (g_bound_tex == t) g_bound_tex = nullptr;
        if (g_tex == t) g_tex = nullptr;
        if (t->tex.data) linearFree(t->tex.data);
        delete t;
    }
    g_garbage.clear();
}

size_t texture_bytes(const Texture* t) { return t ? t->bytes : 0; }
int texture_width(const Texture* t) { return t ? t->w : 0; }
int texture_height(const Texture* t) { return t ? t->h : 0; }

void init() {
    for (int y = 0; y < 8; y++) {
        for (int x = 0; x < 8; x++) {
            g_morton[y * 8 + x] = (uint8_t)((x & 1) | ((y & 1) << 1) | ((x & 2) << 1) | ((y & 2) << 2) | ((x & 4) << 2) | ((y & 4) << 3));
        }
    }
    gfxInitDefault();
    C3D_Init(C3D_DEFAULT_CMDBUF_SIZE * 2);
    g_top = C3D_RenderTargetCreate(TOP_H, TOP_W, GPU_RB_RGBA8, -1);
    C3D_RenderTargetSetOutput(g_top, GFX_TOP, GFX_LEFT, TRANSFER_FLAGS);
    g_bottom = C3D_RenderTargetCreate(PANEL_H, PANEL_W, GPU_RB_RGBA8, -1);
    C3D_RenderTargetSetOutput(g_bottom, GFX_BOTTOM, GFX_LEFT, TRANSFER_FLAGS);
    C3D_TexInitVRAM(&g_panel.tex, PANEL_CANVAS_W, PANEL_CANVAS_H, GPU_RGBA8);
    C3D_TexSetWrap(&g_panel.tex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
    g_panel.w = PANEL_W;
    g_panel.h = PANEL_H;
    g_panel.iw = 1.0f / PANEL_CANVAS_W;
    g_panel.ih = 1.0f / PANEL_CANVAS_H;
    g_panel_target = C3D_RenderTargetCreateFromTex(&g_panel.tex, GPU_TEXFACE_2D, 0, -1);
    for (int i = 0; i < 2; i++) {
        Texture& c = g_canvas[i];
        C3D_TexInitVRAM(&c.tex, CANVAS, CANVAS, GPU_RGBA8);
        C3D_TexSetWrap(&c.tex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
        c.w = SCREEN_W;
        c.h = SCREEN_H;
        c.iw = c.ih = 1.0f / CANVAS;
        g_canvas_target[i] = C3D_RenderTargetCreateFromTex(&c.tex, GPU_TEXFACE_2D, 0, -1);
    }
    g_dvlb = DVLB_ParseFile((u32*)sprite_shbin, sprite_shbin_size);
    shaderProgramInit(&g_program);
    shaderProgramSetVsh(&g_program, &g_dvlb->DVLE[0]);
    C3D_BindProgram(&g_program);
    g_proj_loc = shaderInstanceGetUniformLocation(g_program.vertexShader, "projection");
    C3D_AttrInfo* attr = C3D_GetAttrInfo();
    AttrInfo_Init(attr);
    AttrInfo_AddLoader(attr, 0, GPU_FLOAT, 2);
    AttrInfo_AddLoader(attr, 1, GPU_FLOAT, 2);
    AttrInfo_AddLoader(attr, 2, GPU_UNSIGNED_BYTE, 4);
    Mtx_Ortho(&g_proj_canvas, 0.0f, CANVAS, CANVAS, 0.0f, 1.0f, -1.0f, true);
    Mtx_OrthoTilt(&g_proj_top, 0.0f, TOP_W, TOP_H, 0.0f, 1.0f, -1.0f, true);
    Mtx_Ortho(&g_proj_panel, 0.0f, PANEL_CANVAS_W, PANEL_CANVAS_H, 0.0f, 1.0f, -1.0f, true);
    Mtx_OrthoTilt(&g_proj_bottom, 0.0f, PANEL_W, PANEL_H, 0.0f, 1.0f, -1.0f, true);
    g_vbuf = (Vtx*)linearAlloc(sizeof(Vtx) * 4 * (MAX_QUADS + 2));
    g_ibuf = (uint16_t*)linearAlloc(sizeof(uint16_t) * 6 * BATCH_QUADS);
    for (int i = 0; i < BATCH_QUADS; i++) {
        uint16_t b = (uint16_t)(i * 4);
        uint16_t* q = &g_ibuf[i * 6];
        q[0] = b;
        q[1] = b + 1;
        q[2] = b + 2;
        q[3] = b;
        q[4] = b + 2;
        q[5] = b + 3;
    }
    GSPGPU_FlushDataCache(g_ibuf, sizeof(uint16_t) * 6 * BATCH_QUADS);
    C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
    C3D_AlphaTest(false, GPU_ALWAYS, 0);
    C3D_CullFace(GPU_CULL_NONE);
}

void shutdown() {
    collect_garbage();
    for (int i = 0; i < 2; i++) {
        C3D_RenderTargetDelete(g_canvas_target[i]);
        C3D_TexDelete(&g_canvas[i].tex);
    }
    C3D_RenderTargetDelete(g_panel_target);
    C3D_TexDelete(&g_panel.tex);
    C3D_RenderTargetDelete(g_bottom);
    C3D_RenderTargetDelete(g_top);
    linearFree(g_vbuf);
    linearFree(g_ibuf);
    shaderProgramFree(&g_program);
    DVLB_Free(g_dvlb);
    C3D_Fini();
    gfxExit();
}

static void apply_blend(Blend b) {
    switch (b) {
        case Blend::Normal: C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ONE_MINUS_SRC_ALPHA, GPU_ONE, GPU_ONE_MINUS_SRC_ALPHA); break;
        case Blend::Add: C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ONE, GPU_ONE, GPU_ONE); break;
        case Blend::Multiply: C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_DST_COLOR, GPU_ONE_MINUS_SRC_ALPHA, GPU_DST_ALPHA, GPU_ONE_MINUS_SRC_ALPHA); break;
        case Blend::Screen: C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ONE_MINUS_SRC_COLOR, GPU_ONE, GPU_ONE_MINUS_SRC_ALPHA); break;
        case Blend::Subtract: C3D_AlphaBlend(GPU_BLEND_REVERSE_SUBTRACT, GPU_BLEND_ADD, GPU_ONE, GPU_ONE, GPU_ZERO, GPU_ONE); break;
        case Blend::Copy: C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO); break;
    }
}

static void apply_tev(Mode mode, int gray) {
    for (int i = 0; i < 4; i++) C3D_TexEnvInit(C3D_GetTexEnv(i));
    C3D_TexEnv* env = C3D_GetTexEnv(0);
    if (mode == Mode::Fill) {
        C3D_TexEnvSrc(env, C3D_Both, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
        C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
        return;
    }
    if (mode == Mode::Texture) {
        C3D_TexEnvSrc(env, C3D_Both, GPU_TEXTURE0, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
        C3D_TexEnvFunc(env, C3D_Both, GPU_MODULATE);
        return;
    }
    const uint32_t third = 0xFF555555u;
    const GPU_TEVOP_RGB channel[3] = {GPU_TEVOP_RGB_SRC_R, GPU_TEVOP_RGB_SRC_G, GPU_TEVOP_RGB_SRC_B};
    for (int i = 0; i < 3; i++) {
        env = C3D_GetTexEnv(i);
        C3D_TexEnvSrc(env, C3D_RGB, GPU_TEXTURE0, GPU_CONSTANT, i ? GPU_PREVIOUS : GPU_PRIMARY_COLOR);
        C3D_TexEnvOpRgb(env, channel[i], GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_COLOR);
        C3D_TexEnvFunc(env, C3D_RGB, i ? GPU_MULTIPLY_ADD : GPU_MODULATE);
        C3D_TexEnvColor(env, third);
    }
    env = C3D_GetTexEnv(0);
    C3D_TexEnvSrc(env, C3D_Alpha, GPU_TEXTURE0, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
    C3D_TexEnvFunc(env, C3D_Alpha, GPU_REPLACE);
    env = C3D_GetTexEnv(3);
    C3D_TexEnvSrc(env, C3D_RGB, GPU_PREVIOUS, GPU_TEXTURE0, GPU_CONSTANT);
    C3D_TexEnvOpRgb(env, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_ALPHA);
    C3D_TexEnvFunc(env, C3D_RGB, GPU_INTERPOLATE);
    C3D_TexEnvColor(env, (uint32_t)gray << 24);
}

static void flush() {
    int n = g_used - g_start;
    if (n <= 0) return;
    if (!g_state_valid || g_bound_blend != g_blend) {
        apply_blend(g_blend);
        g_bound_blend = g_blend;
    }
    if (!g_state_valid || g_bound_mode != g_mode || (g_mode == Mode::Gray && g_bound_gray != g_gray)) {
        apply_tev(g_mode, g_gray);
        g_bound_mode = g_mode;
        g_bound_gray = g_gray;
    }
    if (g_mode != Mode::Fill && (!g_state_valid || g_bound_tex != g_tex || g_bound_smooth != g_smooth)) {
        GPU_TEXTURE_FILTER_PARAM f = g_smooth ? GPU_LINEAR : GPU_NEAREST;
        C3D_TexSetFilter(&g_tex->tex, f, f);
        C3D_TexBind(0, &g_tex->tex);
        g_bound_tex = g_tex;
        g_bound_smooth = g_smooth;
    }
    g_state_valid = true;
    C3D_BufInfo* bi = C3D_GetBufInfo();
    BufInfo_Init(bi);
    BufInfo_Add(bi, g_vbuf + (size_t)g_start * 4, sizeof(Vtx), 3, 0x210);
    C3D_DrawElements(GPU_TRIANGLES, n * 6, C3D_UNSIGNED_SHORT, g_ibuf);
    g_start = g_used;
}

static Vtx* prepare(Mode mode, Texture* tex, Blend blend, bool smooth) {
    if (g_scissor_empty) return nullptr;
    if (g_used >= g_limit) {
        if (!g_overflow) plat::log("gfx: more than %d quads in a frame", MAX_QUADS);
        g_overflow = true;
        return nullptr;
    }
    if (g_used > g_start && (g_mode != mode || g_tex != tex || g_blend != blend || g_smooth != smooth || g_used - g_start >= BATCH_QUADS)) flush();
    g_mode = mode;
    g_tex = tex;
    g_blend = blend;
    g_smooth = smooth;
    return &g_vbuf[(size_t)g_used++ * 4];
}

static void apply_scissor() {
    g_scissor_empty = g_sx1 <= g_sx0 || g_sy1 <= g_sy0;
    if (!g_scissor_empty) C3D_SetScissor(GPU_SCISSOR_NORMAL, g_sx0, g_target_h - g_sy1, g_sx1, g_target_h - g_sy0);
}

static void draw_on_canvas(int idx) {
    g_cur = idx;
    C3D_FrameDrawOn(g_canvas_target[idx]);
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, g_proj_loc, &g_proj_canvas);
}

void begin_frame(uint32_t clear_color) {
    C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
    collect_garbage();
    g_used = g_start = 0;
    g_limit = MAX_QUADS;
    g_in_panel = false;
    g_panel_drawn = false;
    g_target_h = CANVAS;
    g_clip_w = SCREEN_W;
    g_clip_h = SCREEN_H;
    g_state_valid = false;
    g_bound_tex = nullptr;
    C3D_RenderTargetClear(g_top, C3D_CLEAR_COLOR, 0x000000FF, 0);
    C3D_RenderTargetClear(g_canvas_target[0], C3D_CLEAR_COLOR, __builtin_bswap32(clear_color), 0);
    draw_on_canvas(0);
    reset_scissor();
}

void begin_panel(uint32_t clear_color) {
    flush();
    g_in_panel = true;
    g_panel_drawn = true;
    g_target_h = PANEL_CANVAS_H;
    g_clip_w = PANEL_W;
    g_clip_h = PANEL_H;
    C3D_RenderTargetClear(g_panel_target, C3D_CLEAR_COLOR, __builtin_bswap32(clear_color), 0);
    C3D_FrameDrawOn(g_panel_target);
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, g_proj_loc, &g_proj_panel);
    reset_scissor();
}

void end_panel() {
    if (!g_in_panel) return;
    flush();
    g_in_panel = false;
    g_target_h = CANVAS;
    g_clip_w = SCREEN_W;
    g_clip_h = SCREEN_H;
    draw_on_canvas(g_cur);
    reset_scissor();
}

static void present(C3D_RenderTarget* target, const C3D_Mtx* proj) {
    flush();
    C3D_FrameDrawOn(target);
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, g_proj_loc, proj);
    C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0);
    g_scissor_empty = false;
}

void end_frame() {
    end_panel();
    flush();
    g_limit = MAX_QUADS + 2;
    present(g_top, &g_proj_top);
    float x0 = (TOP_W - SCREEN_W) / 2, y0 = (TOP_H - SCREEN_H) / 2;
    sprite(&g_canvas[g_cur], 0, 0, SCREEN_W, SCREEN_H, x0, y0, x0 + SCREEN_W, y0 + SCREEN_H, 0xFFFFFFFFu, Blend::Copy, false);
    if (g_panel_drawn) {
        present(g_bottom, &g_proj_bottom);
        sprite(&g_panel, 0, 0, PANEL_W, PANEL_H, 0, 0, PANEL_W, PANEL_H, 0xFFFFFFFFu, Blend::Copy, false);
    }
    flush();
    GSPGPU_FlushDataCache(g_vbuf, sizeof(Vtx) * 4 * g_used);
    C3D_FrameEnd(0);
}

static const uint32_t* read_back(const Texture& t, int tex_w, int w, int h, std::vector<uint32_t>& out) {
    C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
    C3D_FrameEnd(0);
    const uint32_t* src = (const uint32_t*)t.tex.data;
    out.resize((size_t)w * h);
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            uint32_t p = src[((size_t)(y / 8) * (tex_w / 8) + x / 8) * 64 + g_morton[(y % 8) * 8 + x % 8]];
            out[(size_t)y * w + x] = __builtin_bswap32(p) | 0xFF000000u;
        }
    }
    return out.data();
}

const uint32_t* framebuffer() { return read_back(g_canvas[g_cur], CANVAS, SCREEN_W, SCREEN_H, g_shot); }

const uint32_t* panel_framebuffer() { return read_back(g_panel, PANEL_CANVAS_W, PANEL_W, PANEL_H, g_panel_shot); }

void set_scissor(int x, int y, int w, int h) {
    flush();
    g_sx0 = std::max(0, x);
    g_sy0 = std::max(0, y);
    g_sx1 = std::min(g_clip_w, x + w);
    g_sy1 = std::min(g_clip_h, y + h);
    apply_scissor();
}

void reset_scissor() {
    flush();
    g_sx0 = 0;
    g_sy0 = 0;
    g_sx1 = g_clip_w;
    g_sy1 = g_clip_h;
    apply_scissor();
}

void sprite(Texture* tex, float u0, float v0, float u1, float v1, float x0, float y0, float x1, float y1, uint32_t color, Blend blend, bool smooth) {
    if (!tex || !tex->tex.data) return;
    Vtx* v = prepare(Mode::Texture, tex, blend, smooth);
    if (!v) return;
    float s0 = u0 * tex->iw, s1 = u1 * tex->iw, t0 = tc(tex, v0), t1 = tc(tex, v1);
    v[0] = {x0, y0, s0, t0, color};
    v[1] = {x1, y0, s1, t0, color};
    v[2] = {x1, y1, s1, t1, color};
    v[3] = {x0, y1, s0, t1, color};
}

void draw_quad(Texture* tex, const float uv[8], const float xy[8], uint32_t color, Blend blend, bool smooth) {
    if (!tex || !tex->tex.data) return;
    Vtx* v = prepare(Mode::Texture, tex, blend, smooth);
    if (!v) return;
    for (int k = 0; k < 4; k++) v[k] = {xy[k * 2], xy[k * 2 + 1], uv[k * 2] * tex->iw, tc(tex, uv[k * 2 + 1]), color};
}

void fill(float x0, float y0, float x1, float y1, uint32_t color, Blend blend) {
    Vtx* v = prepare(Mode::Fill, nullptr, blend, false);
    if (!v) return;
    v[0] = {x0, y0, 0, 0, color};
    v[1] = {x1, y0, 0, 0, color};
    v[2] = {x1, y1, 0, 0, color};
    v[3] = {x0, y1, 0, 0, color};
}

void desaturate(float amount) {
    if (amount <= 0) return;
    if (amount > 1) amount = 1;
    if (g_in_panel) return;
    flush();
    if (g_used >= MAX_QUADS) return;
    Texture* src = &g_canvas[g_cur];
    draw_on_canvas(g_cur ^ 1);
    C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0);
    g_scissor_empty = false;
    g_gray = (int)(amount * 255.0f + 0.5f);
    Vtx* v = prepare(Mode::Gray, src, Blend::Copy, false);
    float s = SCREEN_W * src->iw, t0 = tc(src, 0), t1 = tc(src, SCREEN_H);
    v[0] = {0, 0, 0, t0, 0xFFFFFFFFu};
    v[1] = {(float)SCREEN_W, 0, s, t0, 0xFFFFFFFFu};
    v[2] = {(float)SCREEN_W, (float)SCREEN_H, s, t1, 0xFFFFFFFFu};
    v[3] = {0, (float)SCREEN_H, 0, t1, 0xFFFFFFFFu};
    flush();
    apply_scissor();
}

}
