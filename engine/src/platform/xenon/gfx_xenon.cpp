#include "../gfx.h"
#include "../platform.h"

#include <malloc.h>
#include <xenos/edram.h>
#include <xenos/xe.h>
#include <xenos/xenos.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "../common/embedded_shaders.h"

extern "C" {
void Xe_pSetState(struct XenosDevice* xe);
void Xe_pStuff(struct XenosDevice* xe);
void Xe_pSetIndexOffset(struct XenosDevice* xe, int offset);
void Xe_pDrawNonIndexed(struct XenosDevice* xe, int num_points, int primtype);
void Xe_pSyncToDevice(struct XenosDevice* xe, volatile void* data, int len);
void Xe_pSyncFromDevice(struct XenosDevice* xe, volatile void* data, int len);
void Xe_pInvalidateGpuCache(struct XenosDevice* xe, int base, int size);
}

namespace gfx {

struct Texture {
    XenosSurface s;
    int w = 0;
    int h = 0;
    size_t bytes = 0;
};

namespace {

struct Vtx {
    float x, y, u, v;
    uint32_t color;
    float e[4];
};

struct Rect {
    float x, y, w, h;
};

enum Program { PROG_COLOR, PROG_TEXTURE, PROG_GRAY, PROG_PRESENT, PROG_COUNT };

constexpr int MAX_VTX = 4096;
constexpr size_t RING_BYTES = 8u << 20;
constexpr size_t RING_SLACK = 8192;
constexpr int TEX_FORMAT = XE_FMT_8888 | XE_FMT_ARGB;
constexpr int FB_FORMAT = XE_FMT_8888 | XE_FMT_BGRA;
constexpr int EDRAM_TILES = 2048;
constexpr int TILE_W = 80;
constexpr int TILE_H = 16;
constexpr int KICK_DWORDS = 0x8000;
constexpr int SYNC_WPTR = 0x70000;
constexpr double FLIP_GUARD = 0.004;
constexpr u32 REG_COHER_DEST_BASE_1 = 0x2007;
constexpr u32 REG_SCREEN_SCISSOR_TL = 0x200e;

const char* const PIXEL_NAMES[PROG_COUNT] = {"color_f", "texture_f", "gray_f", "present_f"};

const XenosVBFFormat VBF = {4,
                            {{XE_USAGE_POSITION, 0, XE_TYPE_FLOAT2},
                             {XE_USAGE_TEXCOORD, 0, XE_TYPE_FLOAT2},
                             {XE_USAGE_COLOR, 0, XE_TYPE_UBYTE4},
                             {XE_USAGE_TEXCOORD, 1, XE_TYPE_FLOAT4}}};

XenosDevice g_device;
XenosDevice* g_xe = nullptr;
bool g_ready = false;
XenosShader* g_vs = nullptr;
XenosShader* g_ps[PROG_COUNT] = {};
XenosVertexBuffer* g_vb = nullptr;
size_t g_ring_used = 0;
float g_ps_consts[5][4];

Texture g_screen;
int g_screen_pitch = 0;
int g_screen_depth = 0;
bool g_dirty = false;

XenosSurface g_fb[2];
int g_back = 0;
int g_disp_w = SCREEN_W;
int g_disp_h = SCREEN_H;
float g_aspect = 16.0f / 9.0f;
bool g_overscan = false;
int g_band_h = 0;
int g_band_pitch = 0;
int g_band_depth = 0;
double g_last_flip = 0;

uint32_t g_serial = 0;
uint32_t g_synced_serial = 0;

Vtx g_batch[MAX_VTX];
int g_count = 0;
Texture* g_tex = nullptr;
Blend g_blend = Blend::Normal;
bool g_smooth = false;
int g_applied_blend = -1;
bool g_frame_open = false;
bool g_frame_pending = false;
int g_sx0 = 0, g_sy0 = 0, g_sx1 = SCREEN_W, g_sy1 = SCREEN_H;
std::vector<Texture*> g_garbage;

uint32_t from_argb(uint32_t v) { return ((v >> 16) & 255) | (v & 0xFF00) | ((v & 255) << 16) | 0xFF000000u; }

size_t tiled_texel(int x, int y, int pitch) {
    return (size_t)((((y >> 5) * 32 * pitch + ((x >> 5) << 10) + (x & 3) + ((y & 1) << 2) + (((x & 31) >> 2) << 3) + (((y & 31) >> 1) << 6)) ^
                     ((y & 8) << 2)));
}

void put(u32 v) { ((volatile u32*)g_xe->rb_secondary)[g_xe->rb_secondary_wptr++] = v; }

void write_reg(u32 reg, u32 v) {
    put(reg);
    put(v);
}

u32 phys(const void* p) { return (u32)(uintptr_t)p & ~0x80000000u; }

size_t surface_bytes(const XenosSurface& s) { return (size_t)s.wpitch * (size_t)s.hpitch; }

void surface_init(XenosSurface& s, int w, int h, int format, bool tiled) {
    std::memset(&s, 0, sizeof(s));
    s.width = w;
    s.height = h;
    s.wpitch = (w * 4 + 127) & ~127;
    s.hpitch = (h + 31) & ~31;
    s.tiled = tiled ? 1 : 0;
    s.format = format;
    s.bypp = 4;
    s.u_addressing = XE_TEXADDR_CLAMP;
    s.v_addressing = XE_TEXADDR_CLAMP;
}

bool surface_alloc(XenosSurface& s, int w, int h, int format, bool tiled) {
    surface_init(s, w, h, format, tiled);
    s.base = memalign(4096, surface_bytes(s));
    if (!s.base) return false;
    s.ptr = phys(s.base);
    return true;
}

void upload(const XenosSurface& s, size_t bytes) {
    Xe_pSyncToDevice(g_xe, s.base, (int)bytes);
    size_t start = s.ptr & ~(size_t)4095;
    size_t end = (s.ptr + bytes + 4095) & ~(size_t)4095;
    Xe_pInvalidateGpuCache(g_xe, (int)start, (int)(end - start));
    g_serial++;
}

void sync_gpu() {
    Xe_Sync(g_xe);
    g_ring_used = 0;
    g_synced_serial = g_serial;
}

void submit(bool force) {
    if (!force && g_xe->rb_secondary_wptr - g_xe->last_wptr < KICK_DWORDS) return;
    if (g_xe->rb_secondary_wptr > SYNC_WPTR) {
        sync_gpu();
        return;
    }
    Xe_Execute(g_xe);
}

void collect_garbage() {
    for (Texture* t : g_garbage) {
        if (g_tex == t) g_tex = nullptr;
        std::free(t->s.base);
        delete t;
    }
    g_garbage.clear();
}

XenosShader* load_shader(const char* name, bool vertex) {
    const EmbeddedShader* e = embedded_shader(name);
    if (!e) return nullptr;
    XenosShader* sh = Xe_LoadShaderFromMemory(g_xe, (void*)e->data);
    if (!sh) return nullptr;
    Xe_InstantiateShader(g_xe, sh, 0);
    if (vertex) Xe_ShaderApplyVFetchPatches(g_xe, sh, 0, &VBF);
    return sh;
}

bool setup_programs() {
    g_vs = load_shader("sprite_v", true);
    if (!g_vs) return false;
    for (int p = 0; p < PROG_COUNT; p++) {
        g_ps[p] = load_shader(PIXEL_NAMES[p], false);
        if (!g_ps[p]) return false;
    }
    return true;
}

int tiles_per_line(int w) { return ((w + TILE_W - 1) / TILE_W + 1) & ~1; }

bool setup_screen() {
    int tpl = tiles_per_line(SCREEN_W);
    int tiles = tpl * ((SCREEN_H + TILE_H - 1) / TILE_H);
    if (2 * tiles > EDRAM_TILES) return false;
    g_screen_pitch = tpl * TILE_W;
    g_screen_depth = tiles;
    if (!surface_alloc(g_screen.s, SCREEN_W, SCREEN_H, TEX_FORMAT, true)) return false;
    uint32_t* p = (uint32_t*)g_screen.s.base;
    size_t n = surface_bytes(g_screen.s) / 4;
    for (size_t i = 0; i < n; i++) p[i] = 0xFF000000u;
    upload(g_screen.s, surface_bytes(g_screen.s));
    g_screen.w = SCREEN_W;
    g_screen.h = SCREEN_H;
    g_screen.bytes = surface_bytes(g_screen.s);
    return true;
}

void use_layout(int w, int h, int pitch, int color_base, int depth_base, XenosSurface* rt) {
    g_xe->rt = rt;
    g_xe->vp_xres = w;
    g_xe->vp_yres = h;
    g_xe->msaa_samples = 0;
    g_xe->edram_colorformat = 0;
    g_xe->edram_pitch = pitch;
    g_xe->edram_hizpitch = pitch;
    g_xe->edram_color0base = color_base;
    g_xe->edram_depthbase = depth_base;
    g_xe->dirty |= DIRTY_MISC;
}

void use_screen() { use_layout(SCREEN_W, SCREEN_H, g_screen_pitch, 0, g_screen_depth, &g_screen.s); }

bool scissor_empty() { return g_sx1 <= g_sx0 || g_sy1 <= g_sy0; }

void apply_scissor() {
    if (!scissor_empty()) Xe_SetScissor(g_xe, 1, g_sx0, g_sy0, g_sx1, g_sy1);
}

void after_resolve() {
    g_xe->alu_dirty = ~0u;
    g_xe->fetch_dirty = ~0u;
    g_xe->dirty = ~0;
    g_serial++;
}

void resolve_screen() {
    use_screen();
    Xe_ResolveInto(g_xe, &g_screen.s, XE_SOURCE_COLOR, 0);
    after_resolve();
    apply_scissor();
    g_dirty = false;
    submit(false);
}

void choose_display() {
    XenosSurface* def = Xe_GetFramebufferSurface(g_xe);
    g_disp_w = def->width;
    g_disp_h = def->height;
    g_aspect = g_disp_h <= 576 ? 4.0f / 3.0f : (float)g_disp_w / (float)g_disp_h;
    g_overscan = xenos_is_overscan() != 0;
    int tpl = tiles_per_line(g_disp_w);
    g_band_h = std::min(((EDRAM_TILES / (2 * tpl)) * TILE_H) & ~31, g_disp_h);
    g_band_pitch = tpl * TILE_W;
    g_band_depth = tpl * ((g_band_h + TILE_H - 1) / TILE_H);
}

void set_ps_const(int index, float x, float y, float z, float w) {
    g_ps_consts[index][0] = x;
    g_ps_consts[index][1] = y;
    g_ps_consts[index][2] = z;
    g_ps_consts[index][3] = w;
    Xe_SetPixelShaderConstantF(g_xe, index, g_ps_consts[index], 1);
}

void apply_blend(Blend b) {
    if ((int)b == g_applied_blend) return;
    g_applied_blend = (int)b;
    int op = XE_BLENDOP_ADD, src = XE_BLEND_ONE, dst = XE_BLEND_ZERO;
    switch (b) {
        case Blend::Normal: dst = XE_BLEND_INVSRCALPHA; break;
        case Blend::Add: dst = XE_BLEND_ONE; break;
        case Blend::Multiply:
            src = XE_BLEND_DESTCOLOR;
            dst = XE_BLEND_INVSRCALPHA;
            break;
        case Blend::Screen: dst = XE_BLEND_INVSRCCOLOR; break;
        case Blend::Subtract:
            op = XE_BLENDOP_REVSUBTRACT;
            dst = XE_BLEND_ONE;
            break;
        case Blend::Copy: break;
    }
    Xe_SetBlendControl(g_xe, src, op, dst, src, op, dst);
}

void set_fetch(int slot, const XenosSurface& s, bool smooth) {
    u32* d = g_xe->fetch_constants + slot * 6;
    u32 filter = smooth ? 1 : 0;
    d[0] = 2u | ((u32)XE_TEXADDR_CLAMP << 10) | ((u32)XE_TEXADDR_CLAMP << 13) | ((u32)(s.wpitch / 128) << 22) | ((u32)s.tiled << 31);
    d[1] = s.ptr | (u32)s.format;
    d[2] = ((u32)(s.height - 1) << 13) | (u32)(s.width - 1);
    d[3] = 0xc14u | (filter << 19) | (filter << 21) | (2u << 23);
    d[4] = 0;
    d[5] = 0x200u;
    Xe_DirtyFetch(g_xe, slot * 3, 3);
}

void emit(Program prog, const Texture* tex, bool smooth, Blend blend, const Vtx* v, int n, int w, int h, int y0, bool mark) {
    if (n <= 0) return;
    size_t bytes = (size_t)n * sizeof(Vtx);
    size_t at = (g_ring_used + 31) & ~(size_t)31;
    if (at + bytes > RING_BYTES) {
        sync_gpu();
        at = 0;
    }
    g_ring_used = at + bytes;
    void* dst = Xe_VB_Lock(g_xe, g_vb, (int)at, (int)bytes, XE_LOCK_WRITE);
    std::memcpy(dst, v, bytes);
    Xe_VB_Unlock(g_xe, g_vb);
    Xe_SetStreamSource(g_xe, 0, g_vb, (int)at, (int)sizeof(Vtx));
    Xe_SetShader(g_xe, SHADER_TYPE_VERTEX, g_vs, 0);
    Xe_SetShader(g_xe, SHADER_TYPE_PIXEL, g_ps[prog], 0);
    const float xf[8] = {2.0f / w, -2.0f / h, 1.0f, 1.0f, -1.0f - 1.0f / w, 1.0f + (2.0f * y0 + 1.0f) / h, 0.0f, 0.0f};
    Xe_SetVertexShaderConstantF(g_xe, 0, xf, 2);
    apply_blend(blend);
    if (tex) set_fetch(0, tex->s, smooth);
    Xe_pStuff(g_xe);
    Xe_pSetState(g_xe);
    write_reg(REG_COHER_DEST_BASE_1, 0);
    Xe_pSetIndexOffset(g_xe, 0);
    Xe_pDrawNonIndexed(g_xe, n, XE_PRIMTYPE_QUADLIST);
    g_serial++;
    if (mark) g_dirty = true;
    submit(false);
}

Vtx vtx(float x, float y, float u, float v, uint32_t c) {
    Vtx o;
    o.x = x;
    o.y = y;
    o.u = u;
    o.v = v;
    o.color = c;
    o.e[0] = o.e[1] = o.e[2] = o.e[3] = 0.0f;
    return o;
}

void rect_quad(Vtx q[4], float x0, float y0, float x1, float y1, uint32_t color) {
    q[0] = vtx(x0, y0, 0, 0, color);
    q[1] = vtx(x1, y0, 1, 0, color);
    q[2] = vtx(x1, y1, 1, 1, color);
    q[3] = vtx(x0, y1, 0, 1, color);
}

void flush() {
    if (!g_count) return;
    int n = g_count;
    g_count = 0;
    if (!g_frame_open || scissor_empty()) return;
    emit(g_tex ? PROG_TEXTURE : PROG_COLOR, g_tex, g_smooth, g_blend, g_batch, n, SCREEN_W, SCREEN_H, 0, true);
}

Vtx* reserve(Texture* tex, Blend blend, bool smooth) {
    if (g_count && (g_tex != tex || g_blend != blend || g_smooth != smooth || g_count + 4 > MAX_VTX)) flush();
    g_tex = tex;
    g_blend = blend;
    g_smooth = smooth;
    Vtx* v = &g_batch[g_count];
    g_count += 4;
    return v;
}

Vtx texel(const Texture* t, float x, float y, float u, float v, uint32_t c) { return vtx(x, y, t ? u / t->w : 0.0f, t ? v / t->h : 0.0f, c); }

void wait_flip() {
    if (Xe_IsVBlank(g_xe) && plat::time_seconds() - g_last_flip >= FLIP_GUARD) return;
    while (Xe_IsVBlank(g_xe)) {
    }
    while (!Xe_IsVBlank(g_xe)) {
    }
}

Rect present_rect() {
    float aw = (float)g_disp_w, ah = (float)g_disp_h, ax = 0.0f, ay = 0.0f;
    if (g_overscan) {
        ax = (float)(g_disp_w / 28);
        ay = (float)(g_disp_h / 28);
        aw -= 2.0f * ax;
        ah -= 2.0f * ay;
    }
    const float content = (float)SCREEN_W / SCREEN_H;
    float pixel_aspect = g_aspect / ((float)g_disp_w / (float)g_disp_h);
    float h = ah;
    float w = h * content / pixel_aspect;
    if (w > aw) {
        w = aw;
        h = w * pixel_aspect / content;
    }
    return {ax + (aw - w) * 0.5f, ay + (ah - h) * 0.5f, w, h};
}

void present_frame() {
    if (g_dirty) resolve_screen();
    Rect r = present_rect();
    float scale = std::max(1.0f, r.h / SCREEN_H);
    set_ps_const(4, scale, 0.5f - 0.5f / scale, 0.5f, 1.0f);
    Vtx q[4];
    rect_quad(q, r.x, r.y, r.x + r.w, r.y + r.h, 0xFFFFFFFFu);
    XenosSurface& fb = g_fb[g_back];
    for (int y = 0; y < g_disp_h; y += g_band_h) {
        int bh = std::min(g_band_h, g_disp_h - y);
        use_layout(g_disp_w, bh, g_band_pitch, 0, g_band_depth, &fb);
        Xe_SetScissor(g_xe, 1, 0, 0, g_disp_w, bh);
        Vtx black[4];
        rect_quad(black, 0, (float)y, (float)g_disp_w, (float)(y + bh), 0xFF000000u);
        emit(PROG_COLOR, nullptr, false, Blend::Copy, black, 4, g_disp_w, bh, y, false);
        emit(PROG_PRESENT, &g_screen, true, Blend::Copy, q, 4, g_disp_w, bh, y, false);
        XenosSurface band = fb;
        band.height = bh;
        band.ptr = fb.ptr + (u32)(y * fb.wpitch);
        band.base = (uint8_t*)fb.base + (size_t)y * fb.wpitch;
        Xe_ResolveInto(g_xe, &band, XE_SOURCE_COLOR, 0);
        after_resolve();
    }
    sync_gpu();
    wait_flip();
    Xe_SetFrameBufferSurface(g_xe, &fb);
    g_last_flip = plat::time_seconds();
    g_back ^= 1;
    use_screen();
    apply_scissor();
}

void present() {
    if (!g_frame_pending || !g_ready) return;
    present_frame();
    g_frame_pending = false;
}

}

void reserve(size_t) {}
void* alloc(size_t bytes) { return memalign(4096, bytes ? bytes : 64); }
void release(void* p) { std::free(p); }

Texture* texture_create(int w, int h, int stride, int rows, Format fmt, bool tiled, void* pixels, uint32_t* palette) {
    Texture* t = new Texture();
    t->w = w;
    t->h = h;
    if (g_ready && fmt == Format::ARGB8888 && !tiled && pixels && w > 0 && h > 0 && stride >= w && rows >= h) {
        if (stride % 32 == 0 && ((uintptr_t)pixels & 4095) == 0) {
            surface_init(t->s, w, h, TEX_FORMAT, false);
            t->s.wpitch = stride * 4;
            t->s.base = pixels;
            t->s.ptr = phys(pixels);
            t->bytes = (size_t)stride * 4 * rows;
            pixels = nullptr;
        } else if (surface_alloc(t->s, w, h, TEX_FORMAT, false)) {
            for (int y = 0; y < h; y++) std::memcpy((uint8_t*)t->s.base + (size_t)y * t->s.wpitch, (const uint8_t*)pixels + (size_t)y * stride * 4, (size_t)w * 4);
            t->bytes = surface_bytes(t->s);
        } else {
            plat::log("gfx: out of video memory for %dx%d", w, h);
        }
        if (t->s.base) upload(t->s, (size_t)t->s.wpitch * h);
    }
    release(pixels);
    release(palette);
    return t;
}

void texture_destroy(Texture* t) {
    if (!t) return;
    if (t->s.base) g_garbage.push_back(t);
    else delete t;
}

size_t texture_bytes(const Texture* t) { return t ? t->bytes : 0; }
int texture_width(const Texture* t) { return t ? t->w : 0; }
int texture_height(const Texture* t) { return t ? t->h : 0; }

void init() {
    xenos_init(VIDEO_MODE_AUTO);
    g_xe = &g_device;
    Xe_Init(g_xe);
    Xe_SetRenderTarget(g_xe, Xe_GetFramebufferSurface(g_xe));
    choose_display();
    if (!setup_programs()) {
        plat::log("gfx: shader setup failed");
        return;
    }
    g_vb = Xe_CreateVertexBuffer(g_xe, (int)(RING_BYTES + RING_SLACK));
    if (!g_vb) {
        plat::log("gfx: out of memory for the vertex ring");
        return;
    }
    for (int i = 0; i < 2; i++) {
        if (!surface_alloc(g_fb[i], g_disp_w, g_disp_h, FB_FORMAT, true)) {
            plat::log("gfx: out of memory for the display");
            return;
        }
        std::memset(g_fb[i].base, 0, surface_bytes(g_fb[i]));
        Xe_pSyncToDevice(g_xe, g_fb[i].base, (int)surface_bytes(g_fb[i]));
    }
    if (!setup_screen()) {
        plat::log("gfx: screen setup failed");
        return;
    }
    edram_init(g_xe);
    Xe_InvalidateState(g_xe);
    Xe_SetZEnable(g_xe, 0);
    Xe_SetZWrite(g_xe, 0);
    Xe_SetCullMode(g_xe, XE_CULL_NONE);
    Xe_SetStencilEnable(g_xe, 0);
    set_ps_const(0, 1.0f / 3.0f, 1.0f / 3.0f, 1.0f / 3.0f, 1.0f);
    set_ps_const(3, (float)SCREEN_W, (float)SCREEN_H, 1.0f / SCREEN_W, 1.0f / SCREEN_H);
    set_ps_const(4, 1.0f, 0.0f, 0.5f, 1.0f);
    put(0x00010000 | REG_SCREEN_SCISSOR_TL);
    put(0);
    put(8192u | (8192u << 16));
    use_screen();
    apply_scissor();
    sync_gpu();
    while (!Xe_IsVBlank(g_xe)) {
    }
    Xe_SetFrameBufferSurface(g_xe, &g_fb[0]);
    g_last_flip = plat::time_seconds();
    g_back = 1;
    g_ready = true;
    plat::log("video: %dx%d, aspect %.2f%s", g_disp_w, g_disp_h, (double)g_aspect, g_overscan ? ", overscan" : "");
}

void shutdown() {
    if (!g_ready) return;
    present();
    sync_gpu();
}

void begin_frame(uint32_t clear_color) {
    if (!g_ready) return;
    present();
    if (g_synced_serial != g_serial) sync_gpu();
    collect_garbage();
    g_count = 0;
    g_tex = nullptr;
    g_frame_open = true;
    use_screen();
    reset_scissor();
    Vtx q[4];
    rect_quad(q, 0, 0, SCREEN_W, SCREEN_H, 0xFF000000u | (clear_color & 0xFFFFFFu));
    emit(PROG_COLOR, nullptr, false, Blend::Copy, q, 4, SCREEN_W, SCREEN_H, 0, true);
}

void end_frame() {
    if (!g_frame_open) return;
    flush();
    submit(true);
    g_frame_open = false;
    g_frame_pending = true;
}

const uint32_t* framebuffer() {
    static std::vector<uint32_t> px;
    px.assign((size_t)SCREEN_W * SCREEN_H, 0xFF000000u);
    if (!g_ready) return px.data();
    flush();
    if (g_dirty) resolve_screen();
    if (g_synced_serial != g_serial) sync_gpu();
    Xe_pSyncFromDevice(g_xe, g_screen.s.base, (int)surface_bytes(g_screen.s));
    const uint32_t* src = (const uint32_t*)g_screen.s.base;
    int pitch = g_screen.s.wpitch / 4;
    for (int y = 0; y < SCREEN_H; y++) {
        for (int x = 0; x < SCREEN_W; x++) px[(size_t)y * SCREEN_W + x] = from_argb(src[tiled_texel(x, y, pitch)]);
    }
    return px.data();
}

void set_scissor(int x, int y, int w, int h) {
    flush();
    int x1 = x + w, y1 = y + h;
    g_sx0 = std::clamp(x, 0, SCREEN_W);
    g_sy0 = std::clamp(y, 0, SCREEN_H);
    g_sx1 = std::clamp(x1, g_sx0, SCREEN_W);
    g_sy1 = std::clamp(y1, g_sy0, SCREEN_H);
    if (g_xe) apply_scissor();
}

void reset_scissor() {
    flush();
    g_sx0 = 0;
    g_sy0 = 0;
    g_sx1 = SCREEN_W;
    g_sy1 = SCREEN_H;
    if (g_xe) apply_scissor();
}

void sprite(Texture* tex, float u0, float v0, float u1, float v1, float x0, float y0, float x1, float y1, uint32_t color, Blend blend, bool smooth) {
    if (!tex || !tex->s.base) return;
    Vtx* v = reserve(tex, blend, smooth);
    v[0] = texel(tex, x0, y0, u0, v0, color);
    v[1] = texel(tex, x1, y0, u1, v0, color);
    v[2] = texel(tex, x1, y1, u1, v1, color);
    v[3] = texel(tex, x0, y1, u0, v1, color);
}

void draw_quad(Texture* tex, const float uv[8], const float xy[8], uint32_t color, Blend blend, bool smooth) {
    if (!tex || !tex->s.base) return;
    Vtx* v = reserve(tex, blend, smooth);
    for (int k = 0; k < 4; k++) v[k] = texel(tex, xy[k * 2], xy[k * 2 + 1], uv[k * 2], uv[k * 2 + 1], color);
}

void fill(float x0, float y0, float x1, float y1, uint32_t color, Blend blend) {
    Vtx* v = reserve(nullptr, blend, false);
    rect_quad(v, x0, y0, x1, y1, color);
}

void desaturate(float amount) {
    if (amount <= 0 || !g_frame_open) return;
    if (amount > 1) amount = 1;
    flush();
    resolve_screen();
    Xe_SetScissor(g_xe, 1, 0, 0, SCREEN_W, SCREEN_H);
    Vtx q[4];
    rect_quad(q, 0, 0, SCREEN_W, SCREEN_H, 0xFFFFFFFFu);
    for (Vtx& v : q) v.e[0] = amount;
    emit(PROG_GRAY, &g_screen, false, Blend::Copy, q, 4, SCREEN_W, SCREEN_H, 0, true);
    apply_scissor();
}

}
