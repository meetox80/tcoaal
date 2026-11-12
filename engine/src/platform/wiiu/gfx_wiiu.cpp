#include "../gfx.h"
#include "../platform.h"
#include "../common/embedded_shaders.h"

#include <coreinit/cache.h>
#include <gx2/context.h>
#include <gx2/draw.h>
#include <gx2/event.h>
#include <gx2/mem.h>
#include <gx2/registers.h>
#include <gx2/sampler.h>
#include <gx2/shaders.h>
#include <gx2/state.h>
#include <gx2/surface.h>
#include <gx2/texture.h>
#include <gx2/utils.h>
#include <malloc.h>
#include <whb/gfx.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace gfx {

struct Texture {
    GX2Texture t;
    int w = 0;
    int h = 0;
};

namespace {

struct Vtx {
    float x, y, u, v;
    uint32_t color;
    float level;
};

struct Target {
    GX2ColorBuffer cb;
    Texture tex;
};

enum Program { PROG_COLOR, PROG_TEXTURE, PROG_GRAY, PROG_PRESENT, PROG_COUNT };

const char* const PIXEL_NAMES[PROG_COUNT] = {"color_f", "texture_f", "gray_f", "present_f"};

constexpr size_t ALLOC_ALIGN = 256;
constexpr size_t RING_BYTES = 8u << 20;
constexpr size_t QUAD_BYTES = 4 * sizeof(Vtx);
constexpr GX2SurfaceFormat FORMAT = GX2_SURFACE_FORMAT_UNORM_R8_G8_B8_A8;
constexpr uint32_t COMP_RGBA = GX2_COMP_MAP(GX2_SQ_SEL_R, GX2_SQ_SEL_G, GX2_SQ_SEL_B, GX2_SQ_SEL_A);
constexpr uint32_t WHITE = 0xFFFFFFFFu;

bool g_ready = false;
GX2ContextState* g_ctx = nullptr;
GX2VertexShader* g_vs = nullptr;
GX2PixelShader* g_ps[PROG_COUNT] = {};
GX2FetchShader g_fetch;
uint8_t* g_fetch_prog = nullptr;
GX2Sampler g_sampler[2];
uint8_t* g_ring = nullptr;
size_t g_ring_used = 0;
Target g_game[2];
GX2Surface g_staging;
int g_cur = 0;
int g_shown = 0;
bool g_frame_open = false;
bool g_pending = false;
bool g_gpu_busy = false;
float g_present[8] = {(float)SCREEN_W, (float)SCREEN_H, 1.0f / SCREEN_W, 1.0f / SCREEN_H, 1.0f, 0.0f, 0.5f, 1.0f};

int g_count = 0;
Texture* g_tex = nullptr;
Blend g_blend = Blend::Normal;
bool g_smooth = false;
const GX2PixelShader* g_applied_ps = nullptr;
bool g_applied_vs = false;
int g_applied_blend = -1;
int g_sx0 = 0, g_sy0 = 0, g_sx1 = SCREEN_W, g_sy1 = SCREEN_H;
std::vector<Texture*> g_garbage;

bool active() { return g_ready && plat::running(); }

void surface_init(GX2Surface& s, int w, int h, GX2SurfaceUse use, GX2TileMode tile) {
    std::memset(&s, 0, sizeof(s));
    s.dim = GX2_SURFACE_DIM_TEXTURE_2D;
    s.width = (uint32_t)w;
    s.height = (uint32_t)h;
    s.depth = 1;
    s.mipLevels = 1;
    s.format = FORMAT;
    s.aa = GX2_AA_MODE1X;
    s.use = use;
    s.tileMode = tile;
    GX2CalcSurfaceSizeAndAlignment(&s);
}

bool surface_alloc(GX2Surface& s, int w, int h, GX2SurfaceUse use, GX2TileMode tile) {
    surface_init(s, w, h, use, tile);
    s.image = memalign(std::max<uint32_t>(s.alignment, 64), s.imageSize);
    if (!s.image) return false;
    DCFlushRange(s.image, s.imageSize);
    return true;
}

void texture_regs(Texture& t, int w, int h) {
    t.w = w;
    t.h = h;
    t.t.viewFirstMip = 0;
    t.t.viewNumMips = 1;
    t.t.viewFirstSlice = 0;
    t.t.viewNumSlices = 1;
    t.t.compMap = COMP_RGBA;
    GX2InitTextureRegs(&t.t);
}

void wait_gpu() {
    if (!g_gpu_busy) return;
    GX2DrawDone();
    g_gpu_busy = false;
}

void collect_garbage() {
    for (Texture* t : g_garbage) {
        std::free(t->t.surface.image);
        delete t;
    }
    g_garbage.clear();
}

bool setup_programs() {
    const EmbeddedShader* vs = embedded_shader("sprite_v");
    if (!vs || !(g_vs = WHBGfxLoadGFDVertexShader(0, vs->data))) return false;
    for (int p = 0; p < PROG_COUNT; p++) {
        const EmbeddedShader* ps = embedded_shader(PIXEL_NAMES[p]);
        if (!ps || !(g_ps[p] = WHBGfxLoadGFDPixelShader(0, ps->data))) return false;
    }
    const uint32_t vec1 = GX2_SEL_MASK(GX2_SQ_SEL_X, GX2_SQ_SEL_0, GX2_SQ_SEL_0, GX2_SQ_SEL_1);
    const uint32_t vec2 = GX2_SEL_MASK(GX2_SQ_SEL_X, GX2_SQ_SEL_Y, GX2_SQ_SEL_0, GX2_SQ_SEL_1);
    const uint32_t vec4 = GX2_SEL_MASK(GX2_SQ_SEL_X, GX2_SQ_SEL_Y, GX2_SQ_SEL_Z, GX2_SQ_SEL_W);
    GX2AttribStream attribs[4] = {
        {0, 0, offsetof(Vtx, x), GX2_ATTRIB_FORMAT_FLOAT_32_32, GX2_ATTRIB_INDEX_PER_VERTEX, 0, vec2, GX2_ENDIAN_SWAP_DEFAULT},
        {1, 0, offsetof(Vtx, u), GX2_ATTRIB_FORMAT_FLOAT_32_32, GX2_ATTRIB_INDEX_PER_VERTEX, 0, vec2, GX2_ENDIAN_SWAP_DEFAULT},
        {2, 0, offsetof(Vtx, color), GX2_ATTRIB_FORMAT_UNORM_8_8_8_8, GX2_ATTRIB_INDEX_PER_VERTEX, 0, vec4, GX2_ENDIAN_SWAP_8_IN_32},
        {3, 0, offsetof(Vtx, level), GX2_ATTRIB_FORMAT_FLOAT_32, GX2_ATTRIB_INDEX_PER_VERTEX, 0, vec1, GX2_ENDIAN_SWAP_DEFAULT},
    };
    uint32_t size = GX2CalcFetchShaderSizeEx(4, GX2_FETCH_SHADER_TESSELLATION_NONE, GX2_TESSELLATION_MODE_DISCRETE);
    g_fetch_prog = (uint8_t*)memalign(GX2_SHADER_PROGRAM_ALIGNMENT, size);
    if (!g_fetch_prog) return false;
    GX2InitFetchShaderEx(&g_fetch, g_fetch_prog, 4, attribs, GX2_FETCH_SHADER_TESSELLATION_NONE, GX2_TESSELLATION_MODE_DISCRETE);
    GX2Invalidate(GX2_INVALIDATE_MODE_CPU_SHADER, g_fetch_prog, size);
    return true;
}

bool make_target(Target& t) {
    if (!surface_alloc(t.tex.t.surface, SCREEN_W, SCREEN_H, (GX2SurfaceUse)(GX2_SURFACE_USE_TEXTURE | GX2_SURFACE_USE_COLOR_BUFFER), GX2_TILE_MODE_DEFAULT))
        return false;
    texture_regs(t.tex, SCREEN_W, SCREEN_H);
    std::memset(&t.cb, 0, sizeof(t.cb));
    t.cb.surface = t.tex.t.surface;
    t.cb.viewNumSlices = 1;
    GX2InitColorBufferRegs(&t.cb);
    return true;
}

void sample_target(Target& t) {
    GX2Invalidate((GX2InvalidateMode)(GX2_INVALIDATE_MODE_COLOR_BUFFER | GX2_INVALIDATE_MODE_TEXTURE), t.tex.t.surface.image, t.tex.t.surface.imageSize);
}

bool scissor_empty() { return g_sx1 <= g_sx0 || g_sy1 <= g_sy0; }

void scissor_now() {
    if (!scissor_empty()) GX2SetScissor((uint32_t)g_sx0, (uint32_t)g_sy0, (uint32_t)(g_sx1 - g_sx0), (uint32_t)(g_sy1 - g_sy0));
}

void bind_target(Target& t) {
    GX2SetColorBuffer(&t.cb, GX2_RENDER_TARGET_0);
    GX2SetViewport(0.0f, 0.0f, (float)SCREEN_W, (float)SCREEN_H, 0.0f, 1.0f);
}

void base_state() {
    GX2SetShaderMode(GX2_SHADER_MODE_UNIFORM_REGISTER);
    GX2SetCullOnlyControl(GX2_FRONT_FACE_CCW, FALSE, FALSE);
    GX2SetColorControl(GX2_LOGIC_OP_COPY, 1, FALSE, TRUE);
    GX2SetDepthStencilControl(FALSE, FALSE, GX2_COMPARE_FUNC_ALWAYS, FALSE, FALSE, GX2_COMPARE_FUNC_ALWAYS, GX2_STENCIL_FUNCTION_KEEP,
                              GX2_STENCIL_FUNCTION_KEEP, GX2_STENCIL_FUNCTION_KEEP, GX2_COMPARE_FUNC_ALWAYS, GX2_STENCIL_FUNCTION_KEEP,
                              GX2_STENCIL_FUNCTION_KEEP, GX2_STENCIL_FUNCTION_KEEP);
    GX2SetAlphaTest(FALSE, GX2_COMPARE_FUNC_ALWAYS, 0.0f);
    GX2SetTargetChannelMasks(GX2_CHANNEL_MASK_RGBA, (GX2ChannelMask)0, (GX2ChannelMask)0, (GX2ChannelMask)0, (GX2ChannelMask)0, (GX2ChannelMask)0,
                             (GX2ChannelMask)0, (GX2ChannelMask)0);
    g_applied_ps = nullptr;
    g_applied_vs = false;
    g_applied_blend = -1;
}

void restore_context() {
    GX2SetContextState(g_ctx);
    base_state();
    bind_target(g_game[g_cur]);
    scissor_now();
}

void apply_blend(Blend b) {
    int op = GX2_BLEND_COMBINE_MODE_ADD, src = GX2_BLEND_MODE_ONE, dst = GX2_BLEND_MODE_ZERO;
    switch (b) {
        case Blend::Normal: dst = GX2_BLEND_MODE_INV_SRC_ALPHA; break;
        case Blend::Add: dst = GX2_BLEND_MODE_ONE; break;
        case Blend::Multiply:
            src = GX2_BLEND_MODE_DST_COLOR;
            dst = GX2_BLEND_MODE_INV_SRC_ALPHA;
            break;
        case Blend::Screen: dst = GX2_BLEND_MODE_INV_SRC_COLOR; break;
        case Blend::Subtract:
            op = GX2_BLEND_COMBINE_MODE_REV_SUB;
            dst = GX2_BLEND_MODE_ONE;
            break;
        case Blend::Copy: break;
    }
    int key = src | (dst << 8) | (op << 16);
    if (key == g_applied_blend) return;
    g_applied_blend = key;
    GX2SetBlendControl(GX2_RENDER_TARGET_0, (GX2BlendMode)src, (GX2BlendMode)dst, (GX2BlendCombineMode)op, TRUE, (GX2BlendMode)src, (GX2BlendMode)dst,
                       (GX2BlendCombineMode)op);
}

void draw(Program p, Texture* tex, bool smooth, Blend blend, const Vtx* v, int n, int w, int h) {
    if (!g_applied_vs) {
        GX2SetFetchShader(&g_fetch);
        GX2SetVertexShader(g_vs);
        g_applied_vs = true;
    }
    if (g_applied_ps != g_ps[p]) {
        GX2SetPixelShader(g_ps[p]);
        g_applied_ps = g_ps[p];
    }
    const float xform[4] = {2.0f / w, -2.0f / h, -1.0f, 1.0f};
    GX2SetVertexUniformReg(0, 4, xform);
    if (p == PROG_PRESENT) GX2SetPixelUniformReg(0, 8, g_present);
    apply_blend(blend);
    if (tex) {
        GX2SetPixelTexture(&tex->t, 0);
        GX2SetPixelSampler(&g_sampler[smooth ? 1 : 0], 0);
    }
    size_t bytes = (size_t)n * sizeof(Vtx);
    GX2Invalidate(GX2_INVALIDATE_MODE_CPU_ATTRIBUTE_BUFFER, (void*)v, bytes);
    GX2SetAttribBuffer(0, (uint32_t)bytes, sizeof(Vtx), v);
    GX2DrawEx(GX2_PRIMITIVE_MODE_QUADS, (uint32_t)n, 0, 1);
    g_gpu_busy = true;
}

void align_ring() { g_ring_used = (g_ring_used + GX2_VERTEX_BUFFER_ALIGNMENT - 1) & ~(size_t)(GX2_VERTEX_BUFFER_ALIGNMENT - 1); }

void flush() {
    if (!g_count) return;
    const Vtx* v = (const Vtx*)(g_ring + g_ring_used) - g_count;
    draw(g_tex ? PROG_TEXTURE : PROG_COLOR, g_tex, g_smooth, g_blend, v, g_count, SCREEN_W, SCREEN_H);
    g_count = 0;
    align_ring();
}

Vtx* ring_take() {
    if (g_ring_used + QUAD_BYTES > RING_BYTES) {
        flush();
        wait_gpu();
        g_ring_used = 0;
    }
    Vtx* v = (Vtx*)(g_ring + g_ring_used);
    g_ring_used += QUAD_BYTES;
    return v;
}

void put(Vtx& o, float x, float y, float u, float v, uint32_t color, float level) {
    o.x = x;
    o.y = y;
    o.u = u;
    o.v = v;
    o.color = color;
    o.level = level;
}

void quad_now(Program p, Texture* tex, bool smooth, float x0, float y0, float x1, float y1, uint32_t color, float level, int w, int h) {
    flush();
    Vtx* v = ring_take();
    put(v[0], x0, y0, 0, 0, color, level);
    put(v[1], x1, y0, 1, 0, color, level);
    put(v[2], x1, y1, 1, 1, color, level);
    put(v[3], x0, y1, 0, 1, color, level);
    draw(p, tex, smooth, Blend::Copy, v, 4, w, h);
    align_ring();
}

Vtx* reserve(Texture* tex, Blend blend, bool smooth) {
    if (!g_frame_open || scissor_empty()) return nullptr;
    if (g_count && (g_tex != tex || g_blend != blend || g_smooth != smooth)) flush();
    Vtx* v = ring_take();
    g_tex = tex;
    g_blend = blend;
    g_smooth = smooth;
    g_count += 4;
    return v;
}

void present_to(bool tv) {
    GX2ColorBuffer* cb = tv ? WHBGfxGetTVColourBuffer() : WHBGfxGetDRCColourBuffer();
    if (tv) WHBGfxBeginRenderTV();
    else WHBGfxBeginRenderDRC();
    WHBGfxClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    base_state();
    int dw = (int)cb->surface.width, dh = (int)cb->surface.height;
    float w = (float)dw, h = w * SCREEN_H / SCREEN_W;
    if (h > dh) {
        h = (float)dh;
        w = h * SCREEN_W / SCREEN_H;
    }
    float x0 = std::floor((dw - w) * 0.5f), y0 = std::floor((dh - h) * 0.5f);
    float scale = std::max(1.0f, h / SCREEN_H);
    g_present[4] = scale;
    g_present[5] = 0.5f - 0.5f / scale;
    GX2SetViewport(0.0f, 0.0f, (float)dw, (float)dh, 0.0f, 1.0f);
    GX2SetScissor(0, 0, (uint32_t)dw, (uint32_t)dh);
    quad_now(PROG_PRESENT, &g_game[g_shown].tex, true, x0, y0, x0 + w, y0 + h, WHITE, 0.0f, dw, dh);
    if (tv) WHBGfxFinishRenderTV();
    else WHBGfxFinishRenderDRC();
}

void present() {
    if (!g_pending) return;
    g_pending = false;
    sample_target(g_game[g_shown]);
    WHBGfxBeginRender();
    present_to(true);
    present_to(false);
    WHBGfxFinishRender();
    g_gpu_busy = false;
    restore_context();
}

}

void reserve(size_t) {}
void* alloc(size_t bytes) { return memalign(ALLOC_ALIGN, bytes ? bytes : ALLOC_ALIGN); }
void release(void* p) { std::free(p); }

Texture* texture_create(int w, int h, int stride, int rows, Format fmt, bool tiled, void* pixels, uint32_t* palette) {
    Texture* t = nullptr;
    if (active() && fmt == Format::RGBA8888 && !tiled && pixels && w > 0 && h > 0 && stride >= w && rows >= h) {
        t = new Texture();
        GX2Surface& s = t->t.surface;
        surface_init(s, w, h, GX2_SURFACE_USE_TEXTURE, GX2_TILE_MODE_LINEAR_ALIGNED);
        size_t src_pitch = (size_t)stride * 4;
        if (s.pitch == (uint32_t)stride && s.imageSize <= src_pitch * rows && (uintptr_t)pixels % std::max<uint32_t>(s.alignment, 1) == 0) {
            s.image = pixels;
            pixels = nullptr;
        } else if ((s.image = memalign(std::max<uint32_t>(s.alignment, 64), s.imageSize))) {
            uint8_t* dst = (uint8_t*)s.image;
            const uint8_t* src = (const uint8_t*)pixels;
            size_t pitch = (size_t)s.pitch * 4;
            for (int y = 0; y < h; y++) std::memcpy(dst + y * pitch, src + y * src_pitch, (size_t)w * 4);
        }
        if (s.image) {
            texture_regs(*t, w, h);
            GX2Invalidate(GX2_INVALIDATE_MODE_CPU_TEXTURE, s.image, s.imageSize);
        } else {
            plat::log("gfx: out of memory for a %dx%d texture", w, h);
            delete t;
            t = nullptr;
        }
    }
    release(pixels);
    release(palette);
    return t;
}

void texture_destroy(Texture* t) {
    if (t) g_garbage.push_back(t);
}

size_t texture_bytes(const Texture* t) { return t ? t->t.surface.imageSize : 0; }
int texture_width(const Texture* t) { return t ? t->w : 0; }
int texture_height(const Texture* t) { return t ? t->h : 0; }

void init() {
    if (!WHBGfxInit()) {
        plat::log("gfx: WHBGfxInit failed");
        return;
    }
    g_ctx = (GX2ContextState*)memalign(GX2_CONTEXT_STATE_ALIGNMENT, sizeof(GX2ContextState));
    if (!g_ctx) {
        plat::log("gfx: out of memory for the context state");
        return;
    }
    GX2SetupContextStateEx(g_ctx, TRUE);
    GX2SetContextState(g_ctx);
    if (!setup_programs()) {
        plat::log("gfx: shader setup failed");
        return;
    }
    GX2InitSampler(&g_sampler[0], GX2_TEX_CLAMP_MODE_CLAMP, GX2_TEX_XY_FILTER_MODE_POINT);
    GX2InitSampler(&g_sampler[1], GX2_TEX_CLAMP_MODE_CLAMP, GX2_TEX_XY_FILTER_MODE_LINEAR);
    g_ring = (uint8_t*)memalign(GX2_VERTEX_BUFFER_ALIGNMENT, RING_BYTES);
    if (!g_ring || !make_target(g_game[0]) || !make_target(g_game[1])) {
        plat::log("gfx: out of memory for the vertex ring and render targets");
        return;
    }
    g_cur = 0;
    restore_context();
    g_ready = true;
    GX2ColorBuffer* tv = WHBGfxGetTVColourBuffer();
    GX2ColorBuffer* drc = WHBGfxGetDRCColourBuffer();
    plat::log("gfx: tv %ux%u, gamepad %ux%u", (unsigned)tv->surface.width, (unsigned)tv->surface.height, (unsigned)drc->surface.width,
              (unsigned)drc->surface.height);
}

void shutdown() {
    if (!g_ready) return;
    wait_gpu();
    g_ready = false;
}

void begin_frame(uint32_t clear_color) {
    if (!active()) return;
    present();
    wait_gpu();
    collect_garbage();
    g_ring_used = 0;
    g_count = 0;
    g_tex = nullptr;
    g_cur = 0;
    g_sx0 = g_sy0 = 0;
    g_sx1 = SCREEN_W;
    g_sy1 = SCREEN_H;
    g_frame_open = true;
    restore_context();
    quad_now(PROG_COLOR, nullptr, false, 0, 0, SCREEN_W, SCREEN_H, 0xFF000000u | clear_color, 0.0f, SCREEN_W, SCREEN_H);
}

void end_frame() {
    if (!g_frame_open) return;
    flush();
    GX2Flush();
    g_frame_open = false;
    g_pending = true;
    g_shown = g_cur;
}

const uint32_t* framebuffer() {
    static std::vector<uint32_t> px;
    px.assign((size_t)SCREEN_W * SCREEN_H, 0xFF000000u);
    if (!active()) return px.data();
    if (!g_staging.image && !surface_alloc(g_staging, SCREEN_W, SCREEN_H, GX2_SURFACE_USE_TEXTURE, GX2_TILE_MODE_LINEAR_ALIGNED)) return px.data();
    GX2Surface& src = g_game[g_shown].tex.t.surface;
    GX2Invalidate(GX2_INVALIDATE_MODE_COLOR_BUFFER, src.image, src.imageSize);
    GX2CopySurface(&src, 0, 0, &g_staging, 0, 0);
    GX2DrawDone();
    g_gpu_busy = false;
    DCInvalidateRange(g_staging.image, g_staging.imageSize);
    restore_context();
    for (int y = 0; y < SCREEN_H; y++) {
        const uint8_t* p = (const uint8_t*)g_staging.image + (size_t)y * g_staging.pitch * 4;
        uint32_t* out = &px[(size_t)y * SCREEN_W];
        for (int x = 0; x < SCREEN_W; x++, p += 4) out[x] = rgba(p[0], p[1], p[2], 255);
    }
    return px.data();
}

void set_scissor(int x, int y, int w, int h) {
    flush();
    g_sx0 = std::clamp(x, 0, SCREEN_W);
    g_sy0 = std::clamp(y, 0, SCREEN_H);
    g_sx1 = std::clamp(x + w, g_sx0, SCREEN_W);
    g_sy1 = std::clamp(y + h, g_sy0, SCREEN_H);
    if (g_frame_open) scissor_now();
}

void reset_scissor() { set_scissor(0, 0, SCREEN_W, SCREEN_H); }

void sprite(Texture* tex, float u0, float v0, float u1, float v1, float x0, float y0, float x1, float y1, uint32_t color, Blend blend, bool smooth) {
    if (!tex) return;
    Vtx* v = reserve(tex, blend, smooth);
    if (!v) return;
    float iw = 1.0f / tex->w, ih = 1.0f / tex->h;
    put(v[0], x0, y0, u0 * iw, v0 * ih, color, 0.0f);
    put(v[1], x1, y0, u1 * iw, v0 * ih, color, 0.0f);
    put(v[2], x1, y1, u1 * iw, v1 * ih, color, 0.0f);
    put(v[3], x0, y1, u0 * iw, v1 * ih, color, 0.0f);
}

void draw_quad(Texture* tex, const float uv[8], const float xy[8], uint32_t color, Blend blend, bool smooth) {
    if (!tex) return;
    Vtx* v = reserve(tex, blend, smooth);
    if (!v) return;
    float iw = 1.0f / tex->w, ih = 1.0f / tex->h;
    for (int k = 0; k < 4; k++) put(v[k], xy[k * 2], xy[k * 2 + 1], uv[k * 2] * iw, uv[k * 2 + 1] * ih, color, 0.0f);
}

void fill(float x0, float y0, float x1, float y1, uint32_t color, Blend blend) {
    Vtx* v = reserve(nullptr, blend, false);
    if (!v) return;
    put(v[0], x0, y0, 0, 0, color, 0.0f);
    put(v[1], x1, y0, 0, 0, color, 0.0f);
    put(v[2], x1, y1, 0, 0, color, 0.0f);
    put(v[3], x0, y1, 0, 0, color, 0.0f);
}

void desaturate(float amount) {
    if (!g_frame_open || amount <= 0) return;
    if (amount > 1) amount = 1;
    flush();
    Target& src = g_game[g_cur];
    sample_target(src);
    g_cur ^= 1;
    bind_target(g_game[g_cur]);
    GX2SetScissor(0, 0, SCREEN_W, SCREEN_H);
    quad_now(PROG_GRAY, &src.tex, false, 0, 0, SCREEN_W, SCREEN_H, WHITE, amount, SCREEN_W, SCREEN_H);
    scissor_now();
}

}
