#include "../gfx.h"
#include "../platform.h"

#include <malloc.h>
#include <ppu_intrinsics.h>
#include <rsx/rsx.h>
#include <sysutil/video.h>
#include <unistd.h>

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <utility>
#include <vector>

extern const unsigned char sprite_vp[];
extern const unsigned char sprite_fp[];
extern const unsigned char gray_fp[];

namespace gfx {

struct Texture {
    int w = 0;
    int h = 0;
    uint32_t pitch = 0;
    void* mem = nullptr;
    uint32_t offset = 0;
    size_t bytes = 0;
};

namespace {

struct Vtx {
    float x, y, u, v, r, g, b, a;
};

struct Target {
    void* mem = nullptr;
    uint32_t offset = 0;
    uint32_t pitch = 0;
    int w = 0;
    int h = 0;
    uint8_t format = GCM_SURFACE_A8R8G8B8;
};

struct Garbage {
    void* mem;
    uint32_t frame;
};

constexpr int SLOTS = 3;
constexpr int SLOT_VERTS = 131072;
constexpr uint32_t CMD_SIZE = 0x800000;
constexpr uint32_t HOST_SIZE = 0x900000;
constexpr uint32_t CMD_RESERVE = 0x200000;
constexpr uint8_t LABEL = 200;
constexpr uint32_t DISPLAY_ALIGN = 1u << 20;
constexpr double FLIP_WAIT_S = 0.1;
constexpr uint32_t REMAP = (GCM_TEXTURE_REMAP_TYPE_REMAP << GCM_TEXTURE_REMAP_TYPE_B_SHIFT) |
                           (GCM_TEXTURE_REMAP_TYPE_REMAP << GCM_TEXTURE_REMAP_TYPE_G_SHIFT) |
                           (GCM_TEXTURE_REMAP_TYPE_REMAP << GCM_TEXTURE_REMAP_TYPE_R_SHIFT) |
                           (GCM_TEXTURE_REMAP_TYPE_REMAP << GCM_TEXTURE_REMAP_TYPE_A_SHIFT) |
                           (GCM_TEXTURE_REMAP_COLOR_B << GCM_TEXTURE_REMAP_COLOR_B_SHIFT) |
                           (GCM_TEXTURE_REMAP_COLOR_G << GCM_TEXTURE_REMAP_COLOR_G_SHIFT) |
                           (GCM_TEXTURE_REMAP_COLOR_R << GCM_TEXTURE_REMAP_COLOR_R_SHIFT) |
                           (GCM_TEXTURE_REMAP_COLOR_A << GCM_TEXTURE_REMAP_COLOR_A_SHIFT);

gcmContextData* g_ctx = nullptr;
void* g_host = nullptr;
Target g_display[2];
Target g_game[2];
uint32_t g_depth_offset = 0;
uint32_t g_depth_pitch = 0;
int g_disp_w = SCREEN_W;
int g_disp_h = SCREEN_H;
float g_aspect = 16.0f / 9.0f;
float g_safe = 1.0f;
int g_fb = 0;
bool g_first_flip = true;
int g_cur = 0;

rsxVertexProgram* g_vp = nullptr;
void* g_vp_ucode = nullptr;
rsxProgramConst* g_xform = nullptr;
rsxFragmentProgram* g_fp_sprite = nullptr;
rsxFragmentProgram* g_fp_gray = nullptr;
uint32_t g_fp_sprite_offset = 0;
uint32_t g_fp_gray_offset = 0;
int g_unit = 0;
int g_attr_pos = 0;
int g_attr_color = 3;
int g_attr_uv = 8;

Vtx* g_vb[SLOTS];
uint32_t g_vb_offset[SLOTS];
uint32_t g_slot_frame[SLOTS];
volatile uint32_t* g_label = nullptr;
uint32_t g_frame = 0;
int g_slot = 0;
int g_count = 0;
int g_batch = 0;
bool g_overflow = false;

Texture g_white;
Texture g_screen;
std::vector<Garbage> g_garbage;

Texture* g_tex = nullptr;
Blend g_blend = Blend::Normal;
bool g_smooth = false;
const Texture* g_bound_tex = nullptr;
bool g_bound_smooth = false;
Blend g_bound_blend = Blend::Normal;
bool g_state_valid = false;
int g_sx0 = 0, g_sy0 = 0, g_sx1 = SCREEN_W, g_sy1 = SCREEN_H;

uint32_t g_cmd_begin = 0;
uint32_t g_cmd_bytes = 0;

uint32_t align(uint32_t v, uint32_t a) { return (v + a - 1) & ~(a - 1); }

uint32_t cmd_offset(const u32* p) {
    u32 off = 0;
    rsxAddressToOffset(p, &off);
    return off;
}

void setup_commands() {
    g_cmd_begin = cmd_offset(g_ctx->begin);
    g_cmd_bytes = CMD_SIZE - (g_cmd_begin & 0xFFFFF) - 64;
    g_ctx->end = g_ctx->begin + g_cmd_bytes / 4;
    plat::log("gfx: commands at 0x%x, %u bytes", (unsigned)g_cmd_begin, (unsigned)g_cmd_bytes);
}

void make_room() {
    gcmControlRegister volatile* ctrl = gcmGetControlRegister();
    uint32_t used = (uint32_t)((g_ctx->current - g_ctx->begin) * 4);
    if (used + CMD_RESERVE > g_cmd_bytes) {
        rsxSetJumpCommand(g_ctx, g_cmd_begin);
        __sync();
        ctrl->put = g_cmd_begin;
        g_ctx->current = g_ctx->begin;
        used = 0;
    }
    for (;;) {
        uint32_t get = ctrl->get;
        if (get <= g_cmd_begin + used || get >= g_cmd_begin + used + CMD_RESERVE) break;
        usleep(50);
    }
}

bool done(uint32_t frame) { return (int32_t)(*g_label - frame) >= 0; }

void wait_frame(uint32_t frame) {
    while (!done(frame)) usleep(100);
}

void make_target(Target& t, int w, int h, uint8_t format, uint32_t alignment = 64) {
    t.w = w;
    t.h = h;
    t.format = format;
    t.pitch = align((uint32_t)w * 4, 64);
    t.mem = rsxMemalign(alignment, t.pitch * (uint32_t)h);
    rsxAddressToOffset(t.mem, &t.offset);
}

void configure_video() {
    videoState state;
    videoResolution res;
    int id = VIDEO_RESOLUTION_720;
    std::memset(&state, 0, sizeof(state));
    if (videoGetState(0, 0, &state) == 0) {
        id = state.displayMode.resolution;
        if (state.displayMode.aspect == VIDEO_ASPECT_4_3) g_aspect = 4.0f / 3.0f;
    }
    if (videoGetResolution(id, &res) != 0) {
        id = VIDEO_RESOLUTION_720;
        videoGetResolution(id, &res);
    }
    videoConfiguration cfg;
    std::memset(&cfg, 0, sizeof(cfg));
    cfg.resolution = (u8)id;
    cfg.format = VIDEO_BUFFER_FORMAT_XRGB;
    cfg.aspect = VIDEO_ASPECT_AUTO;
    cfg.pitch = align((uint32_t)res.width * 4, 64);
    if (videoConfigure(0, &cfg, nullptr, 0) != 0 && id != VIDEO_RESOLUTION_720) {
        id = VIDEO_RESOLUTION_720;
        videoGetResolution(id, &res);
        cfg.resolution = (u8)id;
        cfg.pitch = align((uint32_t)res.width * 4, 64);
        videoConfigure(0, &cfg, nullptr, 0);
        g_aspect = 16.0f / 9.0f;
    }
    g_disp_w = res.width;
    g_disp_h = res.height;
    g_safe = id == VIDEO_RESOLUTION_480 || id == VIDEO_RESOLUTION_576 ? 0.9f : 1.0f;
    plat::log("video: %dx%d, %s", g_disp_w, g_disp_h, g_aspect < 1.5f ? "4:3" : "16:9");
}

void wait_idle() {
    uint32_t v = ++g_frame;
    rsxSetWriteBackendLabel(g_ctx, LABEL, v);
    rsxFlushBuffer(g_ctx);
    wait_frame(v);
}

rsxFragmentProgram* load_fp(const unsigned char* blob, uint32_t& offset) {
    rsxFragmentProgram* fp = (rsxFragmentProgram*)blob;
    void* ucode = nullptr;
    u32 size = 0;
    rsxFragmentProgramGetUCode(fp, &ucode, &size);
    void* mem = rsxMemalign(64, size);
    std::memcpy(mem, ucode, size);
    rsxAddressToOffset(mem, &offset);
    return fp;
}

int attrib_index(const char* name, int fallback) {
    rsxProgramAttrib* a = rsxVertexProgramGetAttrib(g_vp, name);
    return a ? (int)a->index : fallback;
}

void set_target(const Target& t) {
    gcmSurface sf;
    std::memset(&sf, 0, sizeof(sf));
    sf.colorFormat = t.format;
    sf.colorTarget = GCM_SURFACE_TARGET_0;
    for (int i = 0; i < 4; i++) {
        sf.colorLocation[i] = GCM_LOCATION_RSX;
        sf.colorPitch[i] = 64;
    }
    sf.colorOffset[0] = t.offset;
    sf.colorPitch[0] = t.pitch;
    sf.depthFormat = GCM_SURFACE_ZETA_Z16;
    sf.depthLocation = GCM_LOCATION_RSX;
    sf.depthOffset = g_depth_offset;
    sf.depthPitch = g_depth_pitch;
    sf.type = GCM_SURFACE_TYPE_LINEAR;
    sf.antiAlias = GCM_SURFACE_CENTER_1;
    sf.width = (u16)t.w;
    sf.height = (u16)t.h;
    rsxSetSurface(g_ctx, &sf);
    float scale[4] = {t.w * 0.5f, t.h * -0.5f, 0.5f, 0.0f};
    float offset[4] = {t.w * 0.5f, t.h * 0.5f, 0.5f, 0.0f};
    rsxSetViewport(g_ctx, 0, 0, (u16)t.w, (u16)t.h, 0.0f, 1.0f, scale, offset);
    for (int i = 0; i < 8; i++) rsxSetViewportClip(g_ctx, (u8)i, (u16)t.w, (u16)t.h);
    float xform[4] = {2.0f / t.w, -2.0f / t.h, -1.0f, 1.0f};
    rsxSetVertexProgramParameter(g_ctx, g_vp, g_xform, xform);
}

void set_static_state() {
    rsxSetColorMask(g_ctx, GCM_COLOR_MASK_B | GCM_COLOR_MASK_G | GCM_COLOR_MASK_R | GCM_COLOR_MASK_A);
    rsxSetColorMaskMrt(g_ctx, 0);
    rsxSetDepthTestEnable(g_ctx, GCM_FALSE);
    rsxSetDepthWriteEnable(g_ctx, GCM_FALSE);
    rsxSetCullFaceEnable(g_ctx, GCM_FALSE);
    rsxSetLogicOpEnable(g_ctx, GCM_FALSE);
    rsxSetBlendEnable(g_ctx, GCM_TRUE);
    rsxSetZMinMaxControl(g_ctx, GCM_FALSE, GCM_TRUE, GCM_FALSE);
    rsxSetUserClipPlaneControl(g_ctx, GCM_USER_CLIP_PLANE_DISABLE, GCM_USER_CLIP_PLANE_DISABLE, GCM_USER_CLIP_PLANE_DISABLE,
                               GCM_USER_CLIP_PLANE_DISABLE, GCM_USER_CLIP_PLANE_DISABLE, GCM_USER_CLIP_PLANE_DISABLE);
    rsxLoadVertexProgram(g_ctx, g_vp, g_vp_ucode);
    uint32_t base = g_vb_offset[g_slot];
    rsxBindVertexArrayAttrib(g_ctx, (u8)g_attr_pos, 0, base + offsetof(Vtx, x), sizeof(Vtx), 2, GCM_VERTEX_DATA_TYPE_F32, GCM_LOCATION_RSX);
    rsxBindVertexArrayAttrib(g_ctx, (u8)g_attr_uv, 0, base + offsetof(Vtx, u), sizeof(Vtx), 2, GCM_VERTEX_DATA_TYPE_F32, GCM_LOCATION_RSX);
    rsxBindVertexArrayAttrib(g_ctx, (u8)g_attr_color, 0, base + offsetof(Vtx, r), sizeof(Vtx), 4, GCM_VERTEX_DATA_TYPE_F32, GCM_LOCATION_RSX);
}

void apply_blend(Blend b) {
    switch (b) {
        case Blend::Normal: rsxSetBlendFunc(g_ctx, GCM_ONE, GCM_ONE_MINUS_SRC_ALPHA, GCM_ONE, GCM_ONE_MINUS_SRC_ALPHA); break;
        case Blend::Add: rsxSetBlendFunc(g_ctx, GCM_ONE, GCM_ONE, GCM_ONE, GCM_ONE); break;
        case Blend::Multiply: rsxSetBlendFunc(g_ctx, GCM_DST_COLOR, GCM_ONE_MINUS_SRC_ALPHA, GCM_DST_COLOR, GCM_ONE_MINUS_SRC_ALPHA); break;
        case Blend::Screen: rsxSetBlendFunc(g_ctx, GCM_ONE, GCM_ONE_MINUS_SRC_COLOR, GCM_ONE, GCM_ONE_MINUS_SRC_COLOR); break;
        case Blend::Subtract: rsxSetBlendFunc(g_ctx, GCM_ONE, GCM_ONE, GCM_ONE, GCM_ONE); break;
        case Blend::Copy: rsxSetBlendFunc(g_ctx, GCM_ONE, GCM_ZERO, GCM_ONE, GCM_ZERO); break;
    }
    uint16_t eq = b == Blend::Subtract ? GCM_FUNC_REVERSE_SUBTRACT : GCM_FUNC_ADD;
    rsxSetBlendEquation(g_ctx, eq, eq);
}

void bind_texture(const Texture* t, bool smooth) {
    gcmTexture tex;
    std::memset(&tex, 0, sizeof(tex));
    tex.format = GCM_TEXTURE_FORMAT_A8R8G8B8 | GCM_TEXTURE_FORMAT_LIN;
    tex.mipmap = 1;
    tex.dimension = GCM_TEXTURE_DIMS_2D;
    tex.cubemap = GCM_FALSE;
    tex.remap = REMAP;
    tex.width = (u16)t->w;
    tex.height = (u16)t->h;
    tex.depth = 1;
    tex.location = GCM_LOCATION_RSX;
    tex.pitch = t->pitch;
    tex.offset = t->offset;
    rsxLoadTexture(g_ctx, (u8)g_unit, &tex);
    rsxTextureControl(g_ctx, (u8)g_unit, GCM_TRUE, 0 << 8, 12 << 8, GCM_TEXTURE_MAX_ANISO_1);
    u8 filter = smooth ? GCM_TEXTURE_LINEAR : GCM_TEXTURE_NEAREST;
    rsxTextureFilter(g_ctx, (u8)g_unit, 0, filter, filter, GCM_TEXTURE_CONVOLUTION_QUINCUNX);
    rsxTextureWrapMode(g_ctx, (u8)g_unit, GCM_TEXTURE_CLAMP_TO_EDGE, GCM_TEXTURE_CLAMP_TO_EDGE, GCM_TEXTURE_CLAMP_TO_EDGE, 0,
                       GCM_TEXTURE_ZFUNC_LESS, 0);
}

void flush() {
    if (g_count <= g_batch) return;
    const Texture* t = g_tex ? g_tex : &g_white;
    if (!g_state_valid || g_bound_blend != g_blend) {
        apply_blend(g_blend);
        g_bound_blend = g_blend;
    }
    if (!g_state_valid || g_bound_tex != t || g_bound_smooth != g_smooth) {
        bind_texture(t, g_smooth);
        g_bound_tex = t;
        g_bound_smooth = g_smooth;
    }
    g_state_valid = true;
    rsxDrawVertexArray(g_ctx, GCM_TYPE_QUADS, (u32)g_batch, (u32)(g_count - g_batch));
    g_batch = g_count;
}

Vtx* reserve(Texture* tex, Blend blend, bool smooth) {
    if (g_count > g_batch && (g_tex != tex || g_blend != blend || g_smooth != smooth)) flush();
    g_tex = tex;
    g_blend = blend;
    g_smooth = smooth;
    if (g_count + 4 > SLOT_VERTS) {
        if (!g_overflow) plat::log("gfx: vertex buffer full");
        g_overflow = true;
        return nullptr;
    }
    Vtx* v = &g_vb[g_slot][g_count];
    g_count += 4;
    return v;
}

void put(Vtx& v, float x, float y, float u, float w, const Texture* t, uint32_t c) {
    v.x = x;
    v.y = y;
    v.u = t && t->w ? u / t->w : 0.0f;
    v.v = t && t->h ? w / t->h : 0.0f;
    v.r = (c & 255) * (1.0f / 255.0f);
    v.g = ((c >> 8) & 255) * (1.0f / 255.0f);
    v.b = ((c >> 16) & 255) * (1.0f / 255.0f);
    v.a = (c >> 24) * (1.0f / 255.0f);
}

void apply_scissor() { rsxSetScissor(g_ctx, (u16)g_sx0, (u16)g_sy0, (u16)(g_sx1 - g_sx0), (u16)(g_sy1 - g_sy0)); }

void collect_garbage() {
    size_t k = 0;
    for (const Garbage& g : g_garbage) {
        if (done(g.frame)) rsxFree(g.mem);
        else g_garbage[k++] = g;
    }
    g_garbage.resize(k);
}

void full_quad(Texture* src, float x0, float y0, float x1, float y1, uint32_t color, bool smooth) {
    Vtx* v = reserve(src, Blend::Copy, smooth);
    if (!v) return;
    put(v[0], x0, y0, 0, 0, src, color);
    put(v[1], x1, y0, (float)src->w, 0, src, color);
    put(v[2], x1, y1, (float)src->w, (float)src->h, src, color);
    put(v[3], x0, y1, 0, (float)src->h, src, color);
}

void screen_texture(const Target& t) {
    g_screen.w = t.w;
    g_screen.h = t.h;
    g_screen.pitch = t.pitch;
    g_screen.mem = t.mem;
    g_screen.offset = t.offset;
}

}

void reserve(size_t) {}
void* alloc(size_t bytes) { return memalign(128, bytes ? bytes : 16); }
void release(void* p) { free(p); }

Texture* texture_create(int w, int h, int stride, int rows, Format fmt, bool swizzled, void* pixels, uint32_t* palette) {
    Texture* t = new Texture();
    t->w = w;
    t->h = h;
    t->pitch = (uint32_t)stride * 4;
    size_t bytes = (size_t)t->pitch * (size_t)rows;
    if (fmt == Format::ARGB8888 && !swizzled && pixels && w > 0 && h > 0) {
        t->mem = rsxMemalign(128, (u32)bytes);
        if (t->mem) {
            std::memcpy(t->mem, pixels, bytes);
            rsxAddressToOffset(t->mem, &t->offset);
            t->bytes = bytes;
        } else {
            plat::log("gfx: out of video memory for %dx%d", w, h);
        }
    }
    release(pixels);
    release(palette);
    return t;
}

void texture_destroy(Texture* t) {
    if (!t) return;
    if (g_tex == t) {
        flush();
        g_tex = nullptr;
    }
    if (g_bound_tex == t) g_bound_tex = nullptr;
    if (t->mem) g_garbage.push_back({t->mem, g_frame});
    delete t;
}

size_t texture_bytes(const Texture* t) { return t ? t->bytes : 0; }
int texture_width(const Texture* t) { return t ? t->w : 0; }
int texture_height(const Texture* t) { return t ? t->h : 0; }

void init() {
    g_host = memalign(1024 * 1024, HOST_SIZE);
    rsxInit(&g_ctx, CMD_SIZE, HOST_SIZE, g_host);
    g_label = (volatile uint32_t*)gcmGetLabelAddress(LABEL);
    *g_label = 0;
    setup_commands();
    configure_video();
    wait_idle();
    gcmSetFlipMode(GCM_FLIP_VSYNC);
    for (int i = 0; i < 2; i++) {
        make_target(g_display[i], g_disp_w, g_disp_h, GCM_SURFACE_X8R8G8B8, DISPLAY_ALIGN);
        std::memset(g_display[i].mem, 0, (size_t)g_display[i].pitch * g_disp_h);
        gcmSetDisplayBuffer((u8)i, g_display[i].offset, g_display[i].pitch, (u32)g_disp_w, (u32)g_disp_h);
        make_target(g_game[i], SCREEN_W, SCREEN_H, GCM_SURFACE_A8R8G8B8);
    }
    int dw = std::max(g_disp_w, SCREEN_W), dh = std::max(g_disp_h, SCREEN_H);
    g_depth_pitch = align((uint32_t)dw * 2, 64);
    void* depth = rsxMemalign(64, g_depth_pitch * (uint32_t)dh);
    rsxAddressToOffset(depth, &g_depth_offset);

    g_vp = (rsxVertexProgram*)sprite_vp;
    u32 size = 0;
    rsxVertexProgramGetUCode(g_vp, &g_vp_ucode, &size);
    g_xform = rsxVertexProgramGetConst(g_vp, "xform");
    g_attr_pos = attrib_index("position", 0);
    g_attr_color = attrib_index("color", 3);
    g_attr_uv = attrib_index("texcoord", 8);
    g_fp_sprite = load_fp(sprite_fp, g_fp_sprite_offset);
    g_fp_gray = load_fp(gray_fp, g_fp_gray_offset);
    rsxProgramAttrib* unit = rsxFragmentProgramGetAttrib(g_fp_sprite, "texture");
    g_unit = unit ? unit->index : 0;

    for (int i = 0; i < SLOTS; i++) {
        g_vb[i] = (Vtx*)rsxMemalign(128, SLOT_VERTS * sizeof(Vtx));
        rsxAddressToOffset(g_vb[i], &g_vb_offset[i]);
        g_slot_frame[i] = 0;
    }

    g_white.w = 1;
    g_white.h = 1;
    g_white.pitch = 64;
    g_white.mem = rsxMemalign(128, 64);
    std::memset(g_white.mem, 0xFF, 64);
    rsxAddressToOffset(g_white.mem, &g_white.offset);
}

bool flip_done() {
    if (g_first_flip) return true;
    double start = plat::time_seconds();
    while (gcmGetFlipStatus() != 0) {
        if (plat::time_seconds() - start > FLIP_WAIT_S) return false;
        usleep(200);
    }
    return true;
}

void shutdown() {
    if (!g_ctx) return;
    flip_done();
    rsxFinish(g_ctx, 1);
}

void begin_frame(uint32_t clear_color) {
    g_frame++;
    g_slot = (int)(g_frame % SLOTS);
    wait_frame(g_slot_frame[g_slot]);
    g_slot_frame[g_slot] = g_frame;
    make_room();
    collect_garbage();
    g_count = 0;
    g_batch = 0;
    g_overflow = false;
    g_state_valid = false;
    g_bound_tex = nullptr;
    g_tex = nullptr;
    g_cur = 0;
    set_static_state();
    rsxLoadFragmentProgramLocation(g_ctx, g_fp_sprite, g_fp_sprite_offset, GCM_LOCATION_RSX);
    set_target(g_game[g_cur]);
    reset_scissor();
    uint32_t c = clear_color;
    rsxSetClearColor(g_ctx, ((c & 255) << 16) | (c & 0xFF00) | ((c >> 16) & 255) | 0xFF000000u);
    rsxClearSurface(g_ctx, GCM_CLEAR_R | GCM_CLEAR_G | GCM_CLEAR_B | GCM_CLEAR_A);
}

void end_frame() {
    flush();
    if (!flip_done()) {
        g_tex = nullptr;
        g_bound_tex = nullptr;
        rsxSetWriteBackendLabel(g_ctx, LABEL, g_frame);
        rsxFlushBuffer(g_ctx);
        return;
    }
    const Target& disp = g_display[g_fb];
    set_target(disp);
    rsxSetScissor(g_ctx, 0, 0, (u16)disp.w, (u16)disp.h);
    rsxSetClearColor(g_ctx, 0xFF000000u);
    rsxClearSurface(g_ctx, GCM_CLEAR_R | GCM_CLEAR_G | GCM_CLEAR_B | GCM_CLEAR_A);
    rsxInvalidateTextureCache(g_ctx, GCM_INVALIDATE_TEXTURE);
    screen_texture(g_game[g_cur]);
    float w = disp.w * g_safe, h = disp.h * g_safe;
    const float content = (float)SCREEN_W / SCREEN_H;
    if (g_aspect < content) h *= g_aspect / content;
    float x0 = (disp.w - w) * 0.5f, y0 = (disp.h - h) * 0.5f;
    bool exact = disp.w == SCREEN_W && disp.h == SCREEN_H && g_safe == 1.0f;
    g_bound_tex = nullptr;
    full_quad(&g_screen, x0, y0, x0 + w, y0 + h, 0xFFFFFFFFu, !exact);
    flush();
    g_tex = nullptr;
    g_bound_tex = nullptr;
    rsxSetWriteBackendLabel(g_ctx, LABEL, g_frame);
    gcmResetFlipStatus();
    gcmSetFlip(g_ctx, (u8)g_fb);
    rsxFlushBuffer(g_ctx);
    g_fb ^= 1;
    g_first_flip = false;
}

const uint32_t* framebuffer() {
    static std::vector<uint32_t> px;
    wait_idle();
    const Target& t = g_game[g_cur];
    px.resize((size_t)SCREEN_W * SCREEN_H);
    for (int y = 0; y < SCREEN_H; y++) {
        const uint32_t* row = (const uint32_t*)((const uint8_t*)t.mem + (size_t)y * t.pitch);
        for (int x = 0; x < SCREEN_W; x++) {
            uint32_t argb = row[x];
            px[(size_t)y * SCREEN_W + x] = ((argb >> 16) & 255) | (argb & 0xFF00) | ((argb & 255) << 16) | 0xFF000000u;
        }
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
    apply_scissor();
}

void reset_scissor() {
    flush();
    g_sx0 = 0;
    g_sy0 = 0;
    g_sx1 = SCREEN_W;
    g_sy1 = SCREEN_H;
    apply_scissor();
}

void sprite(Texture* tex, float u0, float v0, float u1, float v1, float x0, float y0, float x1, float y1, uint32_t color, Blend blend, bool smooth) {
    if (!tex || !tex->mem) return;
    Vtx* v = reserve(tex, blend, smooth);
    if (!v) return;
    put(v[0], x0, y0, u0, v0, tex, color);
    put(v[1], x1, y0, u1, v0, tex, color);
    put(v[2], x1, y1, u1, v1, tex, color);
    put(v[3], x0, y1, u0, v1, tex, color);
}

void draw_quad(Texture* tex, const float uv[8], const float xy[8], uint32_t color, Blend blend, bool smooth) {
    if (!tex || !tex->mem) return;
    Vtx* v = reserve(tex, blend, smooth);
    if (!v) return;
    for (int k = 0; k < 4; k++) put(v[k], xy[k * 2], xy[k * 2 + 1], uv[k * 2], uv[k * 2 + 1], tex, color);
}

void fill(float x0, float y0, float x1, float y1, uint32_t color, Blend blend) {
    Vtx* v = reserve(nullptr, blend, false);
    if (!v) return;
    put(v[0], x0, y0, 0, 0, nullptr, color);
    put(v[1], x1, y0, 0, 0, nullptr, color);
    put(v[2], x1, y1, 0, 0, nullptr, color);
    put(v[3], x0, y1, 0, 0, nullptr, color);
}

void desaturate(float amount) {
    if (amount <= 0) return;
    if (amount > 1) amount = 1;
    flush();
    int src = g_cur;
    g_cur ^= 1;
    set_target(g_game[g_cur]);
    rsxSetScissor(g_ctx, 0, 0, SCREEN_W, SCREEN_H);
    rsxInvalidateTextureCache(g_ctx, GCM_INVALIDATE_TEXTURE);
    rsxLoadFragmentProgramLocation(g_ctx, g_fp_gray, g_fp_gray_offset, GCM_LOCATION_RSX);
    screen_texture(g_game[src]);
    g_bound_tex = nullptr;
    int level = (int)(amount * 255.0f + 0.5f);
    full_quad(&g_screen, 0, 0, SCREEN_W, SCREEN_H, (uint32_t)level << 24 | 0xFFFFFFu, false);
    flush();
    g_tex = nullptr;
    g_bound_tex = nullptr;
    rsxLoadFragmentProgramLocation(g_ctx, g_fp_sprite, g_fp_sprite_offset, GCM_LOCATION_RSX);
    apply_scissor();
}

}
