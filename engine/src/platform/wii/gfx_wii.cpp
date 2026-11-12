#include "../gfx.h"
#include "../platform.h"

#include <gccore.h>
#include <malloc.h>
#include <ogc/conf.h>

#include <algorithm>
#include <cstring>
#include <vector>

namespace gfx {

struct Texture {
    int w = 0;
    int h = 0;
    int tw = 0;
    int th = 0;
    void* data = nullptr;
    size_t bytes = 0;
    GXTexObj obj;
};

namespace {

enum class Tev : uint8_t { None, Color, Texture, Gray };

constexpr size_t FIFO_BYTES = 512 * 1024;

GXRModeObj g_mode;
void* g_fifo = nullptr;
void* g_xfb[2] = {nullptr, nullptr};
int g_xfb_index = 1;
bool g_pending = false;
bool g_wide = false;
int g_pic_y = 0;
int g_pic_h = SCREEN_H;
Texture g_screen;
std::vector<Texture*> g_garbage;
bool g_invalidate = false;

Tev g_tev = Tev::None;
Texture* g_bound = nullptr;
bool g_bound_smooth = false;
Blend g_blend = Blend::Normal;
bool g_blend_valid = false;
int g_sx0 = 0, g_sy0 = 0, g_sx1 = SCREEN_W, g_sy1 = SCREEN_H;
bool g_empty = false;

void viewport(int w, int h) {
    Mtx44 proj;
    guOrtho(proj, 0, (f32)h, 0, (f32)w, -1, 1);
    GX_LoadProjectionMtx(proj, GX_ORTHOGRAPHIC);
    GX_SetViewport(0, 0, (f32)w, (f32)h, 0, 1);
}

void stage(u8 n, bool textured) {
    if (textured) GX_SetTevOrder(n, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
    else GX_SetTevOrder(n, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLOR0A0);
    GX_SetTevSwapMode(n, GX_TEV_SWAP0, GX_TEV_SWAP0);
    GX_SetTevColorOp(n, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
    GX_SetTevAlphaOp(n, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
}

void set_tev(Tev mode) {
    if (g_tev == mode) return;
    g_tev = mode;
    switch (mode) {
        case Tev::Color:
            stage(GX_TEVSTAGE0, false);
            GX_SetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_RASC);
            GX_SetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_RASA);
            GX_SetNumTevStages(1);
            break;
        case Tev::Texture:
            stage(GX_TEVSTAGE0, true);
            GX_SetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_TEXC, GX_CC_RASC, GX_CC_ZERO);
            GX_SetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_TEXA, GX_CA_RASA, GX_CA_ZERO);
            GX_SetNumTevStages(1);
            break;
        case Tev::Gray: {
            static const u8 swaps[3] = {GX_TEV_SWAP1, GX_TEV_SWAP2, GX_TEV_SWAP3};
            GX_SetTevKColor(GX_KCOLOR0, GXColor{85, 85, 85, 255});
            for (u8 n = 0; n < 3; n++) {
                stage(n, true);
                GX_SetTevSwapMode(n, GX_TEV_SWAP0, swaps[n]);
                GX_SetTevKColorSel(n, GX_TEV_KCSEL_K0);
                GX_SetTevKAlphaSel(n, GX_TEV_KASEL_K0_A);
                GX_SetTevColorIn(n, GX_CC_ZERO, GX_CC_TEXC, GX_CC_KONST, n ? GX_CC_CPREV : GX_CC_ZERO);
                GX_SetTevAlphaIn(n, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_KONST);
            }
            stage(GX_TEVSTAGE3, true);
            GX_SetTevKColorSel(GX_TEVSTAGE3, GX_TEV_KCSEL_K1);
            GX_SetTevColorIn(GX_TEVSTAGE3, GX_CC_TEXC, GX_CC_CPREV, GX_CC_KONST, GX_CC_ZERO);
            GX_SetTevAlphaIn(GX_TEVSTAGE3, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_APREV);
            GX_SetNumTevStages(4);
            break;
        }
        case Tev::None: break;
    }
}

void set_blend(Blend b) {
    if (g_blend_valid && g_blend == b) return;
    g_blend = b;
    g_blend_valid = true;
    switch (b) {
        case Blend::Normal: GX_SetBlendMode(GX_BM_BLEND, GX_BL_ONE, GX_BL_INVSRCALPHA, GX_LO_CLEAR); break;
        case Blend::Add: GX_SetBlendMode(GX_BM_BLEND, GX_BL_ONE, GX_BL_ONE, GX_LO_CLEAR); break;
        case Blend::Multiply: GX_SetBlendMode(GX_BM_BLEND, GX_BL_DSTCLR, GX_BL_INVSRCALPHA, GX_LO_CLEAR); break;
        case Blend::Screen: GX_SetBlendMode(GX_BM_BLEND, GX_BL_ONE, GX_BL_INVSRCCLR, GX_LO_CLEAR); break;
        case Blend::Subtract: GX_SetBlendMode(GX_BM_SUBTRACT, GX_BL_ONE, GX_BL_ONE, GX_LO_CLEAR); break;
        case Blend::Copy: GX_SetBlendMode(GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_CLEAR); break;
    }
}

void bind(Texture* t, bool smooth) {
    if (g_invalidate) {
        GX_InvalidateTexAll();
        g_invalidate = false;
    }
    if (g_bound == t && g_bound_smooth == smooth) return;
    u8 filter = smooth ? GX_LINEAR : GX_NEAR;
    GX_InitTexObjFilterMode(&t->obj, filter, filter);
    GX_LoadTexObj(&t->obj, GX_TEXMAP0);
    g_bound = t;
    g_bound_smooth = smooth;
}

void use_texture(Texture* t, Blend blend, bool smooth) {
    set_tev(Tev::Texture);
    set_blend(blend);
    bind(t, smooth);
}

void use_color(Blend blend) {
    set_tev(Tev::Color);
    set_blend(blend);
}

inline void vertex(float x, float y, float u, float v, u32 c) {
    GX_Position2f32(x, y);
    GX_Color1u32(c);
    GX_TexCoord2f32(u, v);
}

void rect(float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1, uint32_t color) {
    u32 c = __builtin_bswap32(color);
    GX_Begin(GX_QUADS, GX_VTXFMT0, 4);
    vertex(x0, y0, u0, v0, c);
    vertex(x1, y0, u1, v0, c);
    vertex(x1, y1, u1, v1, c);
    vertex(x0, y1, u0, v1, c);
    GX_End();
}

void init_obj(Texture* t) {
    GX_InitTexObj(&t->obj, t->data, (u16)t->tw, (u16)t->th, GX_TF_RGBA8, GX_CLAMP, GX_CLAMP, GX_FALSE);
    GX_InitTexObjLOD(&t->obj, GX_NEAR, GX_NEAR, 0.0f, 0.0f, 0.0f, GX_FALSE, GX_FALSE, GX_ANISO_1);
}

void grab() {
    GX_SetTexCopySrc(0, 0, SCREEN_W, SCREEN_H);
    GX_SetTexCopyDst(SCREEN_W, SCREEN_H, GX_TF_RGBA8, GX_FALSE);
    GX_CopyTex(g_screen.data, GX_FALSE);
    GX_PixModeSync();
    g_invalidate = true;
}

void apply_scissor() {
    g_empty = g_sx1 <= g_sx0 || g_sy1 <= g_sy0;
    if (!g_empty) GX_SetScissor((u32)g_sx0, (u32)g_sy0, (u32)(g_sx1 - g_sx0), (u32)(g_sy1 - g_sy0));
}

void present() {
    if (!g_pending) return;
    VIDEO_SetNextFramebuffer(g_xfb[g_xfb_index]);
    VIDEO_Flush();
    VIDEO_WaitVSync();
    g_xfb_index ^= 1;
    g_pending = false;
}

void collect_garbage() {
    for (Texture* t : g_garbage) {
        release(t->data);
        delete t;
    }
    g_garbage.clear();
}

}

void reserve(size_t) {}
void* alloc(size_t bytes) { return memalign(32, bytes ? (bytes + 31) & ~(size_t)31 : 32); }
void release(void* p) { free(p); }

Texture* texture_create(int w, int h, int stride, int rows, Format fmt, bool tiled, void* pixels, uint32_t* palette) {
    Texture* t = new Texture();
    t->w = w;
    t->h = h;
    release(palette);
    if (fmt == Format::RGBA8888 && tiled && pixels && w > 0 && h > 0) {
        t->tw = stride;
        t->th = rows;
        t->data = pixels;
        t->bytes = (size_t)stride * (size_t)rows * 4;
        DCFlushRange(t->data, t->bytes);
        init_obj(t);
        g_invalidate = true;
    } else {
        plat::log("gfx: no GX layout for a %dx%d texture in format %d", w, h, (int)fmt);
        release(pixels);
    }
    return t;
}

void texture_destroy(Texture* t) {
    if (t) g_garbage.push_back(t);
}

size_t texture_bytes(const Texture* t) { return t ? t->bytes : 0; }
int texture_width(const Texture* t) { return t ? t->w : 0; }
int texture_height(const Texture* t) { return t ? t->h : 0; }

void init() {
    VIDEO_Init();
    g_mode = *VIDEO_GetPreferredMode(nullptr);
    g_wide = CONF_GetAspectRatio() == CONF_ASPECT_16_9;
    VIDEO_Configure(&g_mode);
    for (void*& x : g_xfb) {
        x = MEM_K0_TO_K1(SYS_AllocateFramebuffer(&g_mode));
        VIDEO_ClearFrameBuffer(&g_mode, x, COLOR_BLACK);
    }
    VIDEO_SetNextFramebuffer(g_xfb[0]);
    VIDEO_SetBlack(false);
    VIDEO_Flush();
    VIDEO_WaitVSync();
    if (g_mode.viTVMode & VI_NON_INTERLACE) VIDEO_WaitVSync();

    int efb_h = g_mode.efbHeight;
    g_pic_h = g_wide ? efb_h : (efb_h * 3 / 4) & ~1;
    g_pic_y = (efb_h - g_pic_h) / 2;
    int standard = g_mode.viTVMode >> 2;
    plat::log("video: %dx%d efb, %d xfb lines, %s %s, %s tv, picture %dx%d at y %d", g_mode.fbWidth, efb_h, g_mode.xfbHeight,
              standard == VI_PAL ? "50 Hz" : "60 Hz", (g_mode.viTVMode & 3) == VI_PROGRESSIVE ? "progressive" : "interlaced", g_wide ? "16:9" : "4:3",
              g_mode.fbWidth, g_pic_h, g_pic_y);

    g_fifo = memalign(32, FIFO_BYTES);
    std::memset(g_fifo, 0, FIFO_BYTES);
    GX_Init(g_fifo, FIFO_BYTES);
    GX_SetCopyClear(GXColor{0, 0, 0, 255}, GX_MAX_Z24);
    GX_SetPixelFmt(GX_PF_RGB8_Z24, GX_ZC_LINEAR);
    GX_SetDither(GX_DISABLE);
    GX_SetDispCopySrc(0, 0, g_mode.fbWidth, g_mode.efbHeight);
    u32 xfb_h = GX_SetDispCopyYScale(GX_GetYScaleFactor(g_mode.efbHeight, g_mode.xfbHeight));
    GX_SetDispCopyDst(g_mode.fbWidth, (u16)xfb_h);
    GX_SetFieldMode(g_mode.field_rendering, g_mode.viHeight == 2 * g_mode.xfbHeight ? GX_ENABLE : GX_DISABLE);
    GX_SetDispCopyGamma(GX_GM_1_0);
    GX_SetCopyFilter(GX_FALSE, g_mode.sample_pattern, GX_FALSE, g_mode.vfilter);

    GX_ClearVtxDesc();
    GX_SetVtxDesc(GX_VA_POS, GX_DIRECT);
    GX_SetVtxDesc(GX_VA_CLR0, GX_DIRECT);
    GX_SetVtxDesc(GX_VA_TEX0, GX_DIRECT);
    GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XY, GX_F32, 0);
    GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
    GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_TEX0, GX_TEX_ST, GX_F32, 0);
    GX_SetNumChans(1);
    GX_SetChanCtrl(GX_COLOR0A0, GX_DISABLE, GX_SRC_REG, GX_SRC_VTX, GX_LIGHTNULL, GX_DF_NONE, GX_AF_NONE);
    GX_SetNumTexGens(1);
    GX_SetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);
    GX_SetTevSwapModeTable(GX_TEV_SWAP0, GX_CH_RED, GX_CH_GREEN, GX_CH_BLUE, GX_CH_ALPHA);
    GX_SetTevSwapModeTable(GX_TEV_SWAP1, GX_CH_RED, GX_CH_RED, GX_CH_RED, GX_CH_ALPHA);
    GX_SetTevSwapModeTable(GX_TEV_SWAP2, GX_CH_GREEN, GX_CH_GREEN, GX_CH_GREEN, GX_CH_ALPHA);
    GX_SetTevSwapModeTable(GX_TEV_SWAP3, GX_CH_BLUE, GX_CH_BLUE, GX_CH_BLUE, GX_CH_ALPHA);
    GX_SetCullMode(GX_CULL_NONE);
    GX_SetClipMode(GX_CLIP_ENABLE);
    GX_SetZMode(GX_FALSE, GX_ALWAYS, GX_FALSE);
    GX_SetZCompLoc(GX_TRUE);
    GX_SetAlphaCompare(GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0);
    GX_SetAlphaUpdate(GX_FALSE);
    GX_SetColorUpdate(GX_TRUE);
    Mtx mv;
    guMtxIdentity(mv);
    GX_LoadPosMtxImm(mv, GX_PNMTX0);
    GX_SetCurrentMtx(GX_PNMTX0);

    g_screen.w = g_screen.tw = SCREEN_W;
    g_screen.h = g_screen.th = SCREEN_H;
    g_screen.bytes = (size_t)SCREEN_W * SCREEN_H * 4;
    g_screen.data = alloc(g_screen.bytes);
    std::memset(g_screen.data, 0, g_screen.bytes);
    DCFlushRange(g_screen.data, g_screen.bytes);
    init_obj(&g_screen);
    viewport(SCREEN_W, SCREEN_H);
    apply_scissor();
}

void shutdown() {
    if (!g_fifo) return;
    GX_DrawDone();
    GX_AbortFrame();
    VIDEO_SetBlack(true);
    VIDEO_Flush();
    VIDEO_WaitVSync();
}

void begin_frame(uint32_t clear_color) {
    present();
    collect_garbage();
    g_bound = nullptr;
    g_tev = Tev::None;
    g_blend_valid = false;
    GX_SetCopyFilter(GX_FALSE, g_mode.sample_pattern, GX_FALSE, g_mode.vfilter);
    viewport(SCREEN_W, SCREEN_H);
    g_sx0 = 0;
    g_sy0 = 0;
    g_sx1 = SCREEN_W;
    g_sy1 = SCREEN_H;
    apply_scissor();
    use_color(Blend::Copy);
    rect(0, 0, SCREEN_W, SCREEN_H, 0, 0, 0, 0, clear_color | 0xFF000000u);
}

void end_frame() {
    grab();
    int fw = g_mode.fbWidth, fh = g_mode.efbHeight;
    viewport(fw, fh);
    GX_SetScissor(0, 0, (u32)fw, (u32)fh);
    bool exact = fw == SCREEN_W && g_pic_h == SCREEN_H;
    use_texture(&g_screen, Blend::Copy, !exact);
    rect(0, (float)g_pic_y, (float)fw, (float)(g_pic_y + g_pic_h), 0, 0, 1, 1, 0xFFFFFFFFu);
    if (g_pic_h < fh) {
        use_color(Blend::Copy);
        rect(0, 0, (float)fw, (float)g_pic_y, 0, 0, 0, 0, 0xFF000000u);
        rect(0, (float)(g_pic_y + g_pic_h), (float)fw, (float)fh, 0, 0, 0, 0, 0xFF000000u);
    }
    GX_SetCopyFilter(g_mode.aa, g_mode.sample_pattern, GX_TRUE, g_mode.vfilter);
    GX_CopyDisp(g_xfb[g_xfb_index], GX_TRUE);
    GX_DrawDone();
    g_pending = true;
}

const uint32_t* framebuffer() {
    static std::vector<uint32_t> px;
    GX_DrawDone();
    DCInvalidateRange(g_screen.data, g_screen.bytes);
    px.resize((size_t)SCREEN_W * SCREEN_H);
    const uint8_t* src = (const uint8_t*)g_screen.data;
    for (int y = 0; y < SCREEN_H; y++) {
        for (int x = 0; x < SCREEN_W; x++) {
            const uint8_t* tile = src + ((size_t)(y >> 2) * (SCREEN_W >> 2) + (size_t)(x >> 2)) * 64;
            size_t at = (size_t)((y & 3) * 4 + (x & 3)) * 2;
            px[(size_t)y * SCREEN_W + x] = rgba(tile[at + 1], tile[32 + at], tile[32 + at + 1]);
        }
    }
    return px.data();
}

void set_scissor(int x, int y, int w, int h) {
    g_sx0 = std::clamp(x, 0, SCREEN_W);
    g_sy0 = std::clamp(y, 0, SCREEN_H);
    g_sx1 = std::clamp(x + w, g_sx0, SCREEN_W);
    g_sy1 = std::clamp(y + h, g_sy0, SCREEN_H);
    apply_scissor();
}

void reset_scissor() {
    g_sx0 = 0;
    g_sy0 = 0;
    g_sx1 = SCREEN_W;
    g_sy1 = SCREEN_H;
    apply_scissor();
}

void sprite(Texture* tex, float u0, float v0, float u1, float v1, float x0, float y0, float x1, float y1, uint32_t color, Blend blend, bool smooth) {
    if (!tex || !tex->data || g_empty || (color == 0 && blend != Blend::Copy)) return;
    use_texture(tex, blend, smooth);
    float su = 1.0f / tex->tw, sv = 1.0f / tex->th;
    rect(x0, y0, x1, y1, u0 * su, v0 * sv, u1 * su, v1 * sv, color);
}

void draw_quad(Texture* tex, const float uv[8], const float xy[8], uint32_t color, Blend blend, bool smooth) {
    if (!tex || !tex->data || g_empty || (color == 0 && blend != Blend::Copy)) return;
    use_texture(tex, blend, smooth);
    float su = 1.0f / tex->tw, sv = 1.0f / tex->th;
    u32 c = __builtin_bswap32(color);
    GX_Begin(GX_QUADS, GX_VTXFMT0, 4);
    for (int k = 0; k < 4; k++) vertex(xy[k * 2], xy[k * 2 + 1], uv[k * 2] * su, uv[k * 2 + 1] * sv, c);
    GX_End();
}

void fill(float x0, float y0, float x1, float y1, uint32_t color, Blend blend) {
    if (g_empty || (color == 0 && blend != Blend::Copy)) return;
    use_color(blend);
    rect(x0, y0, x1, y1, 0, 0, 0, 0, color);
}

void desaturate(float amount) {
    if (amount <= 0) return;
    if (amount > 1) amount = 1;
    grab();
    GX_SetScissor(0, 0, SCREEN_W, SCREEN_H);
    set_tev(Tev::Gray);
    u8 level = (u8)(amount * 255.0f + 0.5f);
    GX_SetTevKColor(GX_KCOLOR1, GXColor{level, level, level, 255});
    set_blend(Blend::Copy);
    bind(&g_screen, false);
    rect(0, 0, SCREEN_W, SCREEN_H, 0, 0, 1, 1, 0xFFFFFFFFu);
    apply_scissor();
}

}
