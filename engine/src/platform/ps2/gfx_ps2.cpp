#include "../gfx.h"
#include "ps2.h"

#include <dma.h>
#include <dma_tags.h>
#include <gif_tags.h>
#include <graph.h>
#include <gs_gp.h>
#include <gs_psm.h>
#include <kernel.h>
#include <malloc.h>

#include <algorithm>
#include <cstring>
#include <utility>
#include <vector>

namespace ps2 {

static volatile uint32_t g_vcount = 0;
static int g_hz = 60;

uint32_t vsync_count() { return g_vcount; }
int refresh_hz() { return g_hz; }

}

namespace gfx {

constexpr int PAGES = 512;
constexpr int RT_TBW = 8;
constexpr int RT_PAGES = 8 * 9;
constexpr int OFS = 2048 * 16;
constexpr int CHAIN_QW = 16384;
constexpr int BLOCKS_X = SCREEN_W / 8;
constexpr int BLOCKS_Y = SCREEN_H / 8;
constexpr int DESAT_SPRITES = BLOCKS_X * 6;
constexpr uint64_t Q_ONE = (uint64_t)0x3F800000u << 32;

struct Texture {
    int w = 0;
    int h = 0;
    int stride = 0;
    int rows = 0;
    int psm = GS_PSM_32;
    int bpp = 4;
    int tbw = 1;
    int tw = 0;
    int th = 0;
    int pages = 0;
    void* pixels = nullptr;
    uint32_t* palette = nullptr;
    size_t bytes = 0;
    int vram = -1;
};

enum class Kind { None, Sprite, Fill, Tri };

static uint64_t g_chain[2][CHAIN_QW * 2] __attribute__((aligned(64)));
static int g_cur = 0;
static uint64_t* g_q = nullptr;
static uint64_t* g_qbase = nullptr;
static uint64_t* g_qend = nullptr;
static uint64_t* g_cnt = nullptr;

static uint64_t* g_tag = nullptr;
static int g_tag_count = 0;
static int g_tag_nreg = 0;
static uint64_t g_tag_regs = 0;

static Texture* g_owner[PAGES];
static int g_tex_start = 0;
static int g_head = 0;
static int g_disp_w = 640;
static int g_disp_h = 448;
static int g_rt = 0;
static int g_tmp = 0;
static int g_clut = 0;
static int g_view_y0 = 0;
static int g_view_y1 = 0;

static bool g_valid = false;
static Kind g_kind = Kind::None;
static Texture* g_bound = nullptr;
static int g_bound_vram = -1;
static bool g_bound_smooth = false;
static Blend g_bound_blend = Blend::Normal;
static int g_sc[4] = {0, SCREEN_W - 1, 0, SCREEN_H - 1};
static std::vector<Texture*> g_garbage;

static int g_vsync_sema = -1;
static volatile int g_vsync_waiting = 0;

static uint64_t g_desat_r[(1 + DESAT_SPRITES * 2) * 2] __attribute__((aligned(64)));
static uint64_t g_desat_g[(1 + DESAT_SPRITES * 2) * 2] __attribute__((aligned(64)));

void reserve(size_t) {}
void* alloc(size_t bytes) { return memalign(64, bytes ? (bytes + 15) & ~(size_t)15 : 16); }
void release(void* p) { free(p); }

static uint32_t phys(const void* p) { return (uint32_t)(uintptr_t)p & 0x0FFFFFFFu; }

static int log2_ceil(int n) {
    int l = 0;
    while ((1 << l) < n) l++;
    return l;
}

static void chain_reset() {
    g_qbase = g_chain[g_cur];
    g_q = g_qbase;
    g_qend = g_qbase + CHAIN_QW * 2;
    g_cnt = nullptr;
}

static void cnt_open() {
    if (g_cnt) return;
    g_cnt = g_q;
    g_q += 2;
}

static void cnt_close() {
    if (!g_cnt) return;
    uint32_t qwc = (uint32_t)((g_q - g_cnt - 2) / 2);
    g_cnt[0] = DMATAG(qwc, 0, DMA_TAG_CNT, 0, 0, 0);
    g_cnt[1] = 0;
    g_cnt = nullptr;
}

static void kick() {
    cnt_close();
    g_q[0] = DMATAG(0, 0, DMA_TAG_END, 0, 0, 0);
    g_q[1] = 0;
    g_q += 2;
    int qwc = (int)((g_q - g_qbase) / 2);
    dma_channel_wait(DMA_CHANNEL_GIF, 0);
    dma_channel_send_chain(DMA_CHANNEL_GIF, g_qbase, qwc, 0, 0);
    g_cur ^= 1;
    chain_reset();
}

static void close_prims();

static void need(int qw) {
    if (g_q + (qw + 4) * 2 > g_qend) {
        close_prims();
        kick();
    }
}

static void ad_begin(int n) {
    cnt_open();
    g_q[0] = GIF_SET_TAG(n, 1, 0, 0, GIF_FLG_PACKED, 1);
    g_q[1] = GIF_REG_AD;
    g_q += 2;
}

static void ad(int reg, uint64_t value) {
    g_q[0] = value;
    g_q[1] = (uint64_t)reg;
    g_q += 2;
}

static void ref(const void* data, int qwc) {
    cnt_close();
    g_q[0] = DMATAG(qwc, 0, DMA_TAG_REF, 0, phys(data), 0);
    g_q[1] = 0;
    g_q += 2;
}

static void image_ref(const void* data, int qwc) {
    const uint8_t* p = (const uint8_t*)data;
    while (qwc > 0) {
        int n = std::min(qwc, GIF_BLOCK_SIZE);
        cnt_open();
        g_q[0] = GIF_SET_TAG(n, 1, 0, 0, GIF_FLG_IMAGE, 0);
        g_q[1] = 0;
        g_q += 2;
        ref(p, n);
        p += (size_t)n * 16;
        qwc -= n;
    }
}

static void close_prims() {
    if (!g_tag) return;
    int words = g_tag_count * g_tag_nreg;
    if (words & 1) *g_q++ = 0;
    g_tag[0] = GIF_SET_TAG(g_tag_count, 1, 0, 0, GIF_FLG_REGLIST, g_tag_nreg);
    g_tag[1] = g_tag_regs;
    g_tag = nullptr;
    g_tag_count = 0;
}

static void open_prims(int nreg, uint64_t regs) {
    cnt_open();
    g_tag = g_q;
    g_q += 2;
    g_tag_count = 0;
    g_tag_nreg = nreg;
    g_tag_regs = regs;
}

static void prim_space(int words) {
    if (g_q + words + 16 > g_qend || g_tag_count >= 32000) {
        int nreg = g_tag_nreg;
        uint64_t regs = g_tag_regs;
        close_prims();
        kick();
        open_prims(nreg, regs);
    }
}

static void ring_evict(Texture* t) {
    if (t->vram < 0) return;
    int n = t->pages + (t->palette ? 1 : 0);
    for (int i = t->vram; i < t->vram + n && i < PAGES; i++) {
        if (g_owner[i] == t) g_owner[i] = nullptr;
    }
    t->vram = -1;
}

static int ring_alloc(int n, Texture* t) {
    if (g_head + n > PAGES) g_head = g_tex_start;
    int p = g_head;
    for (int i = p; i < p + n; i++) {
        if (g_owner[i]) ring_evict(g_owner[i]);
    }
    for (int i = p; i < p + n; i++) g_owner[i] = t;
    g_head += n;
    return p;
}

static void upload(Texture* t) {
    need(16);
    t->vram = ring_alloc(t->pages + (t->palette ? 1 : 0), t);
    ad_begin(4);
    ad(GS_REG_BITBLTBUF, GS_SET_BITBLTBUF(0, 0, 0, t->vram * 32, t->tbw, t->psm));
    ad(GS_REG_TRXPOS, GS_SET_TRXPOS(0, 0, 0, 0, 0));
    ad(GS_REG_TRXREG, GS_SET_TRXREG(t->stride, t->rows));
    ad(GS_REG_TRXDIR, GS_SET_TRXDIR(0));
    image_ref(t->pixels, (int)((size_t)t->stride * t->rows * t->bpp / 16));
    if (t->palette) {
        need(16);
        ad_begin(4);
        ad(GS_REG_BITBLTBUF, GS_SET_BITBLTBUF(0, 0, 0, (t->vram + t->pages) * 32, 1, GS_PSM_32));
        ad(GS_REG_TRXPOS, GS_SET_TRXPOS(0, 0, 0, 0, 0));
        ad(GS_REG_TRXREG, GS_SET_TRXREG(16, 16));
        ad(GS_REG_TRXDIR, GS_SET_TRXDIR(0));
        image_ref(t->palette, 64);
    }
    need(4);
    ad_begin(1);
    ad(GS_REG_TEXFLUSH, 0);
}

Texture* texture_create(int w, int h, int stride, int rows, Format fmt, bool, void* pixels, uint32_t* palette) {
    Texture* t = new Texture();
    t->w = w;
    t->h = h;
    t->stride = stride;
    t->rows = rows;
    t->pixels = pixels;
    t->palette = fmt == Format::T8 ? palette : nullptr;
    switch (fmt) {
        case Format::T8:
            t->psm = GS_PSM_8;
            t->bpp = 1;
            t->tbw = ((stride + 127) / 128) * 2;
            t->pages = (t->tbw / 2) * ((rows + 63) / 64);
            break;
        case Format::RGBA5551:
            t->psm = GS_PSM_16;
            t->bpp = 2;
            t->tbw = (stride + 63) / 64;
            t->pages = t->tbw * ((rows + 63) / 64);
            break;
        default:
            t->psm = GS_PSM_32;
            t->bpp = 4;
            t->tbw = (stride + 63) / 64;
            t->pages = t->tbw * ((rows + 31) / 32);
            break;
    }
    t->tw = log2_ceil(w);
    t->th = log2_ceil(h);
    size_t px = (size_t)stride * rows * t->bpp;
    t->bytes = px + (t->palette ? 1024 : 0);
    SyncDCache(pixels, (uint8_t*)pixels + px);
    if (t->palette) SyncDCache(t->palette, (uint8_t*)t->palette + 1024);
    else if (palette) release(palette);
    return t;
}

void texture_destroy(Texture* t) {
    if (t) g_garbage.push_back(t);
}

static void collect_garbage() {
    for (Texture* t : g_garbage) {
        ring_evict(t);
        if (g_bound == t) g_bound = nullptr;
        release(t->pixels);
        release(t->palette);
        delete t;
    }
    g_garbage.clear();
}

size_t texture_bytes(const Texture* t) { return t ? t->bytes : 0; }
int texture_width(const Texture* t) { return t ? t->w : 0; }
int texture_height(const Texture* t) { return t ? t->h : 0; }

static int vsync_handler(int) {
    ps2::g_vcount = ps2::g_vcount + 1;
    if (g_vsync_waiting) {
        g_vsync_waiting = 0;
        iSignalSema(g_vsync_sema);
    }
    ExitHandler();
    return 0;
}

static void wait_vsync() {
    g_vsync_waiting = 1;
    WaitSema(g_vsync_sema);
}

static inline uint64_t uv(int u, int v) { return GS_SET_UV(u, v); }
static inline uint64_t xyz(int x, int y) { return GS_SET_XYZ(x, y, 0); }

static void build_desat(uint64_t* out, bool green) {
    uint64_t* p = out;
    p[0] = GIF_SET_TAG(DESAT_SPRITES, 1, 0, 0, GIF_FLG_REGLIST, 4);
    p[1] = (uint64_t)GIF_REG_UV | ((uint64_t)GIF_REG_XYZ2 << 4) | ((uint64_t)GIF_REG_UV << 8) | ((uint64_t)GIF_REG_XYZ2 << 12);
    p += 2;
    auto put = [&](int x0, int y0, int u0, int v0, int w) {
        p[0] = uv(u0 * 16 + 8, v0 * 16 + 8);
        p[1] = xyz(OFS + x0 * 16, OFS + y0 * 16);
        p[2] = uv((u0 + w) * 16 + 8, (v0 + 2) * 16 + 8);
        p[3] = xyz(OFS + (x0 + w) * 16, OFS + (y0 + 2) * 16);
        p += 4;
    };
    for (int bx = 0; bx < BLOCKS_X; bx++) {
        for (int k = 0; k < 4; k++) {
            int x0 = bx * 8, u0 = bx * 16, y0 = k * 2, v0 = k * 4 + (green ? 2 : 0);
            bool swap = green ? !(k & 1) : (k & 1);
            if (swap) {
                put(x0, y0, u0 + 4, v0, 4);
                put(x0 + 4, y0, u0, v0, 4);
            } else {
                put(x0, y0, u0, v0, 8);
            }
        }
    }
    SyncDCache(out, p);
}

static void emit_target(int fbp, int fbw, int sw, int sh) {
    need(8);
    ad_begin(3);
    ad(GS_REG_FRAME_1, GS_SET_FRAME(fbp, fbw, GS_PSM_32, 0));
    ad(GS_REG_SCISSOR_1, GS_SET_SCISSOR(0, sw - 1, 0, sh - 1));
    ad(GS_REG_XYOFFSET_1, GS_SET_XYOFFSET(OFS, OFS));
}

static void solid_rect(int x0, int y0, int x1, int y1, uint64_t rgbaq, bool blend) {
    need(8);
    ad_begin(4);
    ad(GS_REG_PRIM, GS_SET_PRIM(GS_PRIM_SPRITE, 0, 0, 0, blend ? 1 : 0, 0, 1, 0, 0));
    ad(GS_REG_RGBAQ, rgbaq);
    ad(GS_REG_XYZ2, xyz(OFS + x0 * 16, OFS + y0 * 16));
    ad(GS_REG_XYZ2, xyz(OFS + x1 * 16, OFS + y1 * 16));
    g_kind = Kind::None;
}

void init() {
    int region = graph_get_region();
    bool pal = region == GRAPH_MODE_PAL;
    ps2::g_hz = pal ? 50 : 60;
    g_disp_w = 640;
    g_disp_h = pal ? 512 : 448;
    int view_h = g_disp_h * 3 / 4;
    g_view_y0 = (g_disp_h - view_h) / 2;
    g_view_y1 = g_view_y0 + view_h;
    graph_set_mode(GRAPH_MODE_INTERLACED, region, GRAPH_MODE_FIELD, GRAPH_ENABLE);
    graph_set_screen(0, 0, g_disp_w, g_disp_h);
    graph_set_bgcolor(0, 0, 0);
    graph_set_framebuffer_filtered(0, g_disp_w, GS_PSM_32, 0, 0);
    graph_enable_output();
    dma_channel_initialize(DMA_CHANNEL_GIF, nullptr, 0);
    dma_channel_fast_waits(DMA_CHANNEL_GIF);
    int disp_pages = (g_disp_w / 64) * (g_disp_h / 32);
    g_rt = disp_pages;
    g_tmp = g_rt + RT_PAGES;
    g_clut = g_tmp + RT_PAGES;
    g_tex_start = g_clut + 1;
    g_head = g_tex_start;
    std::memset(g_owner, 0, sizeof(g_owner));
    build_desat(g_desat_r, false);
    build_desat(g_desat_g, true);
    ee_sema_t s{};
    s.init_count = 0;
    s.max_count = 1;
    g_vsync_sema = CreateSema(&s);
    DIntr();
    AddIntcHandler(INTC_VBLANK_S, vsync_handler, 0);
    EnableIntc(INTC_VBLANK_S);
    EIntr();
    g_cur = 0;
    chain_reset();
    need(16);
    ad_begin(10);
    ad(GS_REG_PRMODECONT, GS_SET_PRMODECONT(1));
    ad(GS_REG_COLCLAMP, GS_SET_COLCLAMP(1));
    ad(GS_REG_DTHE, GS_SET_DTHE(0));
    ad(GS_REG_PABE, GS_SET_PABE(0));
    ad(GS_REG_FBA_1, GS_SET_FBA(0));
    ad(GS_REG_TEXA, GS_SET_TEXA(0x80, 0, 0x80));
    ad(GS_REG_TEST_1, GS_SET_TEST(0, 0, 0, 0, 0, 0, 1, 1));
    ad(GS_REG_ZBUF_1, GS_SET_ZBUF(0, 0, 1));
    ad(GS_REG_TEXCLUT, GS_SET_TEXCLUT(1, 0, 0));
    ad(GS_REG_ALPHA_1, GS_SET_ALPHA(0, 1, 0, 1, 0));
    emit_target(0, g_disp_w / 64, g_disp_w, g_disp_h);
    solid_rect(0, 0, g_disp_w, g_disp_h, GS_SET_RGBAQ(0, 0, 0, 0x80, 0x3F800000), false);
    kick();
    dma_channel_wait(DMA_CHANNEL_GIF, 0);
    g_valid = false;
}

void shutdown() {
    dma_channel_wait(DMA_CHANNEL_GIF, 0);
    DisableIntc(INTC_VBLANK_S);
    graph_shutdown();
}

void begin_frame(uint32_t clear_color) {
    emit_target(g_rt, RT_TBW, SCREEN_W, SCREEN_H);
    solid_rect(0, 0, SCREEN_W, SCREEN_H, GS_SET_RGBAQ(clear_color & 255, (clear_color >> 8) & 255, (clear_color >> 16) & 255, 0x80, 0x3F800000), false);
    g_sc[0] = 0;
    g_sc[1] = SCREEN_W - 1;
    g_sc[2] = 0;
    g_sc[3] = SCREEN_H - 1;
    g_valid = false;
    g_kind = Kind::None;
}

static void final_pass() {
    emit_target(0, g_disp_w / 64, g_disp_w, g_disp_h);
    need(16);
    ad_begin(10);
    ad(GS_REG_TEX0_1, GS_SET_TEX0(g_rt * 32, RT_TBW, GS_PSM_32, 9, 9, 0, 1, 0, 0, 0, 0, 0));
    ad(GS_REG_TEX1_1, GS_SET_TEX1(0, 0, 1, 1, 0, 0, 0));
    ad(GS_REG_CLAMP_1, GS_SET_CLAMP(2, 2, 0, SCREEN_W - 1, 0, SCREEN_H - 1));
    ad(GS_REG_TEXFLUSH, 0);
    ad(GS_REG_PRIM, GS_SET_PRIM(GS_PRIM_SPRITE, 0, 1, 0, 0, 0, 1, 0, 0));
    ad(GS_REG_RGBAQ, GS_SET_RGBAQ(0x80, 0x80, 0x80, 0x80, 0x3F800000));
    ad(GS_REG_UV, uv(0, 0));
    ad(GS_REG_XYZ2, xyz(OFS - 8, OFS + g_view_y0 * 16 - 8));
    ad(GS_REG_UV, uv(SCREEN_W * 16, SCREEN_H * 16));
    ad(GS_REG_XYZ2, xyz(OFS + g_disp_w * 16 - 8, OFS + g_view_y1 * 16 - 8));
}

void end_frame() {
    close_prims();
    kick();
    wait_vsync();
    final_pass();
    kick();
    dma_channel_wait(DMA_CHANNEL_GIF, 0);
    collect_garbage();
    g_valid = false;
    g_kind = Kind::None;
}

const uint32_t* framebuffer() { return nullptr; }

static void emit_scissor() {
    close_prims();
    need(4);
    ad_begin(1);
    ad(GS_REG_SCISSOR_1, GS_SET_SCISSOR(g_sc[0], g_sc[1], g_sc[2], g_sc[3]));
}

void set_scissor(int x, int y, int w, int h) {
    int x1 = std::min(x + w, SCREEN_W), y1 = std::min(y + h, SCREEN_H);
    x = std::max(x, 0);
    y = std::max(y, 0);
    if (x1 <= x || y1 <= y) {
        g_sc[0] = 1;
        g_sc[1] = 0;
        g_sc[2] = 1;
        g_sc[3] = 0;
    } else {
        g_sc[0] = x;
        g_sc[1] = x1 - 1;
        g_sc[2] = y;
        g_sc[3] = y1 - 1;
    }
    emit_scissor();
}

void reset_scissor() {
    g_sc[0] = 0;
    g_sc[1] = SCREEN_W - 1;
    g_sc[2] = 0;
    g_sc[3] = SCREEN_H - 1;
    emit_scissor();
}

static uint64_t alpha_of(Blend b) {
    switch (b) {
        case Blend::Add:
        case Blend::Screen: return GS_SET_ALPHA(0, 2, 0, 1, 0);
        case Blend::Subtract: return GS_SET_ALPHA(2, 0, 0, 1, 0);
        case Blend::Copy: return GS_SET_ALPHA(0, 2, 0, 2, 0);
        default: return GS_SET_ALPHA(0, 1, 0, 1, 0);
    }
}

static void open_kind(Kind kind) {
    uint64_t rgbaq_uv_xyz = (uint64_t)GIF_REG_RGBAQ | ((uint64_t)GIF_REG_UV << 4) | ((uint64_t)GIF_REG_XYZ2 << 8);
    switch (kind) {
        case Kind::Sprite: open_prims(5, rgbaq_uv_xyz | ((uint64_t)GIF_REG_UV << 12) | ((uint64_t)GIF_REG_XYZ2 << 16)); break;
        case Kind::Tri: open_prims(3, rgbaq_uv_xyz); break;
        default: open_prims(3, (uint64_t)GIF_REG_RGBAQ | ((uint64_t)GIF_REG_XYZ2 << 4) | ((uint64_t)GIF_REG_XYZ2 << 8)); break;
    }
}

static void bind(Texture* tex, Blend blend, bool smooth, Kind kind) {
    bool tex_change = tex && (!g_valid || g_bound != tex || g_bound_vram != tex->vram || tex->vram < 0 || g_bound_smooth != smooth);
    bool blend_change = !g_valid || g_bound_blend != blend;
    if (!tex_change && !blend_change && g_valid && g_kind == kind) {
        if (!g_tag) {
            need(4);
            open_kind(kind);
        }
        return;
    }
    close_prims();
    if (tex && tex->vram < 0) upload(tex);
    need(8);
    int n = 1 + (blend_change ? 1 : 0) + (tex_change ? 3 : 0);
    ad_begin(n);
    if (tex_change) {
        int cbp = tex->palette ? (tex->vram + tex->pages) * 32 : 0;
        ad(GS_REG_TEX0_1, GS_SET_TEX0(tex->vram * 32, tex->tbw, tex->psm, tex->tw, tex->th, 1, 0, cbp, 0, 0, 0, tex->palette ? 1 : 0));
        ad(GS_REG_TEX1_1, GS_SET_TEX1(0, 0, smooth ? 1 : 0, smooth ? 1 : 0, 0, 0, 0));
        ad(GS_REG_CLAMP_1, GS_SET_CLAMP(2, 2, 0, tex->w - 1, 0, tex->h - 1));
        g_bound = tex;
        g_bound_vram = tex->vram;
        g_bound_smooth = smooth;
    }
    if (blend_change) {
        ad(GS_REG_ALPHA_1, alpha_of(blend));
        g_bound_blend = blend;
    }
    switch (kind) {
        case Kind::Sprite: ad(GS_REG_PRIM, GS_SET_PRIM(GS_PRIM_SPRITE, 0, 1, 0, 1, 0, 1, 0, 0)); break;
        case Kind::Tri: ad(GS_REG_PRIM, GS_SET_PRIM(GS_PRIM_TRIANGLE, 0, 1, 0, 1, 0, 1, 0, 0)); break;
        default: ad(GS_REG_PRIM, GS_SET_PRIM(GS_PRIM_SPRITE, 0, 0, 0, 1, 0, 1, 0, 0)); break;
    }
    g_valid = true;
    g_kind = kind;
    open_kind(kind);
}

static inline int fx(float x) { return (int)((x + 2047.5f) * 16.0f + 0.5f); }
static inline int fuv(float u) { return std::clamp((int)(u * 16.0f + 0.5f), 0, 16383); }

static uint64_t tex_color(uint32_t c) {
    uint32_t a = c >> 24;
    uint32_t r = c & 255, g = (c >> 8) & 255, b = (c >> 16) & 255;
    if (a == 255) return GS_SET_RGBAQ((r * 128 + 127) / 255, (g * 128 + 127) / 255, (b * 128 + 127) / 255, 0x80, 0) | Q_ONE;
    if (a == 0) return Q_ONE;
    r = std::min<uint32_t>(r * 128 / a, 255);
    g = std::min<uint32_t>(g * 128 / a, 255);
    b = std::min<uint32_t>(b * 128 / a, 255);
    return GS_SET_RGBAQ(r, g, b, (a * 128 + 127) / 255, 0) | Q_ONE;
}

static uint64_t fill_color(uint32_t c) {
    uint32_t a = c >> 24;
    uint32_t r = c & 255, g = (c >> 8) & 255, b = (c >> 16) & 255;
    if (a == 0) return Q_ONE;
    if (a < 255) {
        r = std::min<uint32_t>(r * 255 / a, 255);
        g = std::min<uint32_t>(g * 255 / a, 255);
        b = std::min<uint32_t>(b * 255 / a, 255);
    }
    return GS_SET_RGBAQ(r, g, b, (a * 128 + 127) / 255, 0) | Q_ONE;
}

static bool clip(float& a0, float& a1, float& t0, float& t1, float lo, float hi) {
    if (a1 <= lo || a0 >= hi) return false;
    float span = a1 - a0;
    if (span <= 0) return false;
    float d = (t1 - t0) / span;
    if (a0 < lo) {
        t0 += (lo - a0) * d;
        a0 = lo;
    }
    if (a1 > hi) {
        t1 -= (a1 - hi) * d;
        a1 = hi;
    }
    return true;
}

void sprite(Texture* tex, float u0, float v0, float u1, float v1, float x0, float y0, float x1, float y1, uint32_t color, Blend blend, bool smooth) {
    if (!tex) return;
    if ((color >> 24) == 0 && blend != Blend::Copy) return;
    if (x1 < x0) {
        std::swap(x0, x1);
        std::swap(u0, u1);
    }
    if (y1 < y0) {
        std::swap(y0, y1);
        std::swap(v0, v1);
    }
    if (!clip(x0, x1, u0, u1, -8.0f, SCREEN_W + 8.0f) || !clip(y0, y1, v0, v1, -8.0f, SCREEN_H + 8.0f)) return;
    bind(tex, blend, smooth, Kind::Sprite);
    prim_space(6);
    g_q[0] = tex_color(color);
    g_q[1] = uv(fuv(u0), fuv(v0));
    g_q[2] = xyz(fx(x0), fx(y0));
    g_q[3] = uv(fuv(u1), fuv(v1));
    g_q[4] = xyz(fx(x1), fx(y1));
    g_q += 5;
    g_tag_count++;
}

void draw_quad(Texture* tex, const float uvs[8], const float xy[8], uint32_t color, Blend blend, bool smooth) {
    if (!tex) return;
    if ((color >> 24) == 0 && blend != Blend::Copy) return;
    float minx = xy[0], maxx = xy[0], miny = xy[1], maxy = xy[1];
    for (int i = 1; i < 4; i++) {
        minx = std::min(minx, xy[i * 2]);
        maxx = std::max(maxx, xy[i * 2]);
        miny = std::min(miny, xy[i * 2 + 1]);
        maxy = std::max(maxy, xy[i * 2 + 1]);
    }
    if (maxx <= 0 || maxy <= 0 || minx >= SCREEN_W || miny >= SCREEN_H) return;
    if (minx < -1500 || miny < -1500 || maxx > 2000 || maxy > 2000) return;
    bind(tex, blend, smooth, Kind::Tri);
    prim_space(18);
    uint64_t c = tex_color(color);
    static const int idx[6] = {0, 1, 2, 0, 2, 3};
    for (int i = 0; i < 6; i++) {
        int k = idx[i];
        g_q[0] = c;
        g_q[1] = uv(fuv(uvs[k * 2]), fuv(uvs[k * 2 + 1]));
        g_q[2] = xyz(fx(xy[k * 2]), fx(xy[k * 2 + 1]));
        g_q += 3;
        g_tag_count++;
    }
}

void fill(float x0, float y0, float x1, float y1, uint32_t color, Blend blend) {
    if ((color >> 24) == 0 && blend != Blend::Copy) return;
    if (x1 < x0) std::swap(x0, x1);
    if (y1 < y0) std::swap(y0, y1);
    x0 = std::max(x0, -8.0f);
    y0 = std::max(y0, -8.0f);
    x1 = std::min(x1, SCREEN_W + 8.0f);
    y1 = std::min(y1, SCREEN_H + 8.0f);
    if (x1 <= x0 || y1 <= y0) return;
    bind(nullptr, blend, false, Kind::Fill);
    prim_space(4);
    g_q[0] = fill_color(color);
    g_q[1] = xyz(fx(x0), fx(y0));
    g_q[2] = xyz(fx(x1), fx(y1));
    g_q += 3;
    g_tag_count++;
}

void desaturate(float amount) {
    if (amount <= 0) return;
    if (amount > 1) amount = 1;
    close_prims();
    int level = (int)(amount * 255.0f + 0.5f);
    int keep = 255 - level;
    need(24);
    ad_begin(6);
    ad(GS_REG_BITBLTBUF, GS_SET_BITBLTBUF(g_rt * 32, RT_TBW, GS_PSM_32, g_tmp * 32, RT_TBW, GS_PSM_32));
    ad(GS_REG_TRXPOS, GS_SET_TRXPOS(0, 0, 0, 0, 0));
    ad(GS_REG_TRXREG, GS_SET_TRXREG(SCREEN_W, SCREEN_H));
    ad(GS_REG_TRXDIR, GS_SET_TRXDIR(2));
    ad(GS_REG_TEXFLUSH, 0);
    ad(GS_REG_ALPHA_1, GS_SET_ALPHA(1, 2, 2, 2, (keep * 128 + 127) / 255));
    solid_rect(0, 0, SCREEN_W, SCREEN_H, GS_SET_RGBAQ(0, 0, 0, 0x80, 0x3F800000), true);
    need(80);
    ad_begin(4);
    ad(GS_REG_BITBLTBUF, GS_SET_BITBLTBUF(0, 0, 0, g_clut * 32, 1, GS_PSM_32));
    ad(GS_REG_TRXPOS, GS_SET_TRXPOS(0, 0, 0, 0, 0));
    ad(GS_REG_TRXREG, GS_SET_TRXREG(16, 16));
    ad(GS_REG_TRXDIR, GS_SET_TRXDIR(0));
    g_q[0] = GIF_SET_TAG(64, 1, 0, 0, GIF_FLG_IMAGE, 0);
    g_q[1] = 0;
    g_q += 2;
    uint32_t* clut = (uint32_t*)g_q;
    for (int j = 0; j < 256; j++) {
        int e = (j & ~0x18) | ((j & 0x08) << 1) | ((j & 0x10) >> 1);
        uint32_t v = (uint32_t)(e * level / 765);
        clut[j] = v | (v << 8) | (v << 16) | 0x80000000u;
    }
    g_q += 128;
    ad_begin(5);
    ad(GS_REG_TEXFLUSH, 0);
    ad(GS_REG_TEX0_1, GS_SET_TEX0(g_tmp * 32, RT_TBW * 2, GS_PSM_8, 10, 10, 0, 1, g_clut * 32, 0, 0, 0, 1));
    ad(GS_REG_TEX1_1, GS_SET_TEX1(0, 0, 0, 0, 0, 0, 0));
    ad(GS_REG_ALPHA_1, GS_SET_ALPHA(0, 2, 2, 1, 0x80));
    ad(GS_REG_PRIM, GS_SET_PRIM(GS_PRIM_SPRITE, 0, 1, 0, 1, 0, 1, 0, 0));
    for (int pass = 0; pass < 3; pass++) {
        const uint64_t* list = pass == 1 ? g_desat_g : g_desat_r;
        int wms = pass == 2 ? 3 : 1;
        int minu = pass == 2 ? 0x3F7 : 0;
        int maxu = pass == 2 ? 0x008 : 0;
        for (int by = 0; by < BLOCKS_Y; by++) {
            need(8);
            ad_begin(2);
            ad(GS_REG_CLAMP_1, GS_SET_CLAMP(wms, 3, minu, maxu, 0x00F, by * 16));
            ad(GS_REG_XYOFFSET_1, GS_SET_XYOFFSET(OFS, OFS - by * 8 * 16));
            ref(list, 1 + DESAT_SPRITES * 2);
        }
    }
    need(4);
    ad_begin(1);
    ad(GS_REG_XYOFFSET_1, GS_SET_XYOFFSET(OFS, OFS));
    g_valid = false;
    g_kind = Kind::None;
}

}
