#include "../gfx.h"
#include "../platform.h"
#include "../common/embedded_shaders.h"

#include <malloc.h>
#include <psp2/display.h>
#include <psp2/gxm.h>
#include <psp2/kernel/sysmem.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <map>
#include <unordered_map>
#include <utility>
#include <vector>

namespace gfx {

struct Texture {
    int w = 0;
    int h = 0;
    void* mem = nullptr;
    size_t bytes = 0;
    SceGxmTexture gxm;
};

namespace {

struct Vtx {
    float x, y, u, v;
    uint32_t color;
};

struct Pool {
    uint8_t* base = nullptr;
    uint32_t size = 0;
    std::map<uint32_t, uint32_t> free_blocks;
    std::unordered_map<uint32_t, uint32_t> used_blocks;
};

struct Surface {
    void* mem = nullptr;
    SceGxmColorSurface color;
    SceGxmSyncObject* sync = nullptr;
    Texture tex;
};

struct DisplayData {
    void* address;
};

enum Program { PROG_COLOR, PROG_TEXTURE, PROG_GRAY, PROG_COUNT };

constexpr int MAX_VTX = 4096;
constexpr int BLENDS = 6;
constexpr size_t RING_BYTES = 8u << 20;
constexpr size_t EXTRA_BLOCK = 32u << 20;
constexpr uint32_t POOL_UNIT = 256;
constexpr uint32_t WHITE = 0xFFFFFFFFu;
constexpr uint8_t RGB_MASK = SCE_GXM_COLOR_MASK_R | SCE_GXM_COLOR_MASK_G | SCE_GXM_COLOR_MASK_B;
constexpr float TRANSFORM[4] = {2.0f / SCREEN_W, -2.0f / SCREEN_H, -1.0f, 1.0f};
const char* const FRAGMENT_NAMES[PROG_COUNT] = {"color_f", "texture_f", "gray_f"};

std::vector<Pool*> g_pools;
SceGxmContext* g_ctx = nullptr;
SceGxmShaderPatcher* g_patcher = nullptr;
const SceGxmProgram* g_vertex_program = nullptr;
const SceGxmProgramParameter* g_transform = nullptr;
SceGxmVertexProgram* g_vertex = nullptr;
const SceGxmProgramParameter* g_sampler[PROG_COUNT] = {};
const SceGxmProgramParameter* g_level = nullptr;
SceGxmFragmentProgram* g_frag[PROG_COUNT][BLENDS] = {};
uint16_t* g_quad_indices = nullptr;
uint16_t* g_seq_indices = nullptr;
uint8_t* g_ring = nullptr;
size_t g_ring_used = 0;
SceGxmRenderTarget* g_rt = nullptr;
Surface g_display[2];
Surface g_grab;
Surface* g_target = nullptr;
int g_front = 0;
int g_back = 1;
int g_shown = 0;
bool g_ready = false;
bool g_in_scene = false;
bool g_frame_open = false;

Vtx g_batch[MAX_VTX];
int g_count = 0;
bool g_quads = true;
Texture* g_tex = nullptr;
Blend g_blend = Blend::Normal;
bool g_smooth = false;
int g_sx0 = 0, g_sy0 = 0, g_sx1 = SCREEN_W, g_sy1 = SCREEN_H;
std::vector<Texture*> g_garbage;

uint8_t* map_block(SceKernelMemBlockType type, size_t size) {
    SceUID id = sceKernelAllocMemBlock("gpu", type, size, nullptr);
    if (id < 0) return nullptr;
    void* base = nullptr;
    sceKernelGetMemBlockBase(id, &base);
    if (sceGxmMapMemory(base, size, SCE_GXM_MEMORY_ATTRIB_RW) < 0) {
        sceKernelFreeMemBlock(id);
        return nullptr;
    }
    return (uint8_t*)base;
}

Pool* pool_add(SceKernelMemBlockType type, size_t size, size_t unit) {
    size = (size + unit - 1) / unit * unit;
    uint8_t* base = map_block(type, size);
    if (!base) return nullptr;
    Pool* p = new Pool();
    p->base = base;
    p->size = (uint32_t)size;
    p->free_blocks[0] = p->size;
    g_pools.push_back(p);
    return p;
}

void* pool_take(Pool* p, uint32_t need) {
    auto best = p->free_blocks.end();
    for (auto it = p->free_blocks.begin(); it != p->free_blocks.end(); ++it) {
        if (it->second >= need && (best == p->free_blocks.end() || it->second < best->second)) {
            best = it;
            if (it->second == need) break;
        }
    }
    if (best == p->free_blocks.end()) return nullptr;
    uint32_t at = best->first, len = best->second;
    p->free_blocks.erase(best);
    if (len > need) p->free_blocks[at + need] = len - need;
    p->used_blocks[at] = need;
    return p->base + at;
}

void pool_give(Pool* p, uint32_t at) {
    auto u = p->used_blocks.find(at);
    if (u == p->used_blocks.end()) return;
    uint32_t len = u->second;
    p->used_blocks.erase(u);
    auto next = p->free_blocks.lower_bound(at);
    if (next != p->free_blocks.end() && next->first == at + len) {
        len += next->second;
        next = p->free_blocks.erase(next);
    }
    if (next != p->free_blocks.begin()) {
        auto prev = std::prev(next);
        if (prev->first + prev->second == at) {
            prev->second += len;
            return;
        }
    }
    p->free_blocks[at] = len;
}

void* gpu_take(uint32_t need) {
    for (Pool* p : g_pools) {
        if (void* out = pool_take(p, need)) return out;
    }
    return nullptr;
}

void gpu_free(void* ptr) {
    if (!ptr) return;
    for (Pool* p : g_pools) {
        if ((uint8_t*)ptr >= p->base && (uint8_t*)ptr < p->base + p->size) {
            pool_give(p, (uint32_t)((uint8_t*)ptr - p->base));
            return;
        }
    }
}

void* uncached_block(size_t size) {
    SceUID id = sceKernelAllocMemBlock("gxm", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE, size, nullptr);
    if (id < 0) return nullptr;
    void* base = nullptr;
    sceKernelGetMemBlockBase(id, &base);
    return base;
}

void* mapped_block(size_t size) {
    size = (size + 4095) & ~(size_t)4095;
    void* base = uncached_block(size);
    if (base && sceGxmMapMemory(base, size, SCE_GXM_MEMORY_ATTRIB_READ) < 0) return nullptr;
    return base;
}

void* usse_block(size_t size, bool vertex, unsigned int* offset) {
    size = (size + 4095) & ~(size_t)4095;
    void* base = uncached_block(size);
    if (!base) return nullptr;
    int r = vertex ? sceGxmMapVertexUsseMemory(base, size, offset) : sceGxmMapFragmentUsseMemory(base, size, offset);
    return r < 0 ? nullptr : base;
}

void display_callback(const void* data) {
    const DisplayData* d = (const DisplayData*)data;
    SceDisplayFrameBuf fb{};
    fb.size = sizeof(fb);
    fb.base = d->address;
    fb.pitch = SCREEN_W;
    fb.pixelformat = SCE_DISPLAY_PIXELFORMAT_A8B8G8R8;
    fb.width = SCREEN_W;
    fb.height = SCREEN_H;
    sceDisplaySetFrameBuf(&fb, SCE_DISPLAY_SETBUF_NEXTFRAME);
    sceDisplayWaitVblankStart();
}

void* patcher_alloc(void*, unsigned int size) { return std::malloc(size); }
void patcher_free(void*, void* mem) { std::free(mem); }

bool register_program(const char* name, SceGxmShaderPatcherId* id, const SceGxmProgram** program) {
    const EmbeddedShader* s = embedded_shader(name);
    if (!s || sceGxmProgramCheck((const SceGxmProgram*)s->data) < 0 || sceGxmShaderPatcherRegisterProgram(g_patcher, (const SceGxmProgram*)s->data, id) < 0) {
        plat::log("gfx: shader %s missing or invalid", name);
        return false;
    }
    *program = (const SceGxmProgram*)s->data;
    return true;
}

SceGxmBlendInfo blend_info(Blend b) {
    SceGxmBlendInfo i{};
    i.colorMask = RGB_MASK;
    i.colorFunc = SCE_GXM_BLEND_FUNC_ADD;
    i.colorSrc = SCE_GXM_BLEND_FACTOR_ONE;
    i.colorDst = SCE_GXM_BLEND_FACTOR_ZERO;
    switch (b) {
        case Blend::Normal: i.colorDst = SCE_GXM_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA; break;
        case Blend::Add: i.colorDst = SCE_GXM_BLEND_FACTOR_ONE; break;
        case Blend::Multiply:
            i.colorSrc = SCE_GXM_BLEND_FACTOR_DST_COLOR;
            i.colorDst = SCE_GXM_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            break;
        case Blend::Screen: i.colorDst = SCE_GXM_BLEND_FACTOR_ONE_MINUS_SRC_COLOR; break;
        case Blend::Subtract:
            i.colorFunc = SCE_GXM_BLEND_FUNC_REVERSE_SUBTRACT;
            i.colorDst = SCE_GXM_BLEND_FACTOR_ONE;
            break;
        case Blend::Copy: break;
    }
    i.alphaFunc = i.colorFunc;
    i.alphaSrc = i.colorSrc;
    i.alphaDst = i.colorDst;
    return i;
}

bool setup_programs() {
    SceGxmShaderPatcherId vertex_id;
    if (!register_program("sprite_v", &vertex_id, &g_vertex_program)) return false;
    const SceGxmProgramParameter* pos = sceGxmProgramFindParameterByName(g_vertex_program, "aPosition");
    const SceGxmProgramParameter* uv = sceGxmProgramFindParameterByName(g_vertex_program, "aTexcoord");
    const SceGxmProgramParameter* col = sceGxmProgramFindParameterByName(g_vertex_program, "aColor");
    g_transform = sceGxmProgramFindParameterByName(g_vertex_program, "uTransform");
    if (!pos || !uv || !col || !g_transform) return false;
    SceGxmVertexAttribute attrs[3];
    attrs[0].streamIndex = 0;
    attrs[0].offset = 0;
    attrs[0].format = SCE_GXM_ATTRIBUTE_FORMAT_F32;
    attrs[0].componentCount = 2;
    attrs[0].regIndex = (uint16_t)sceGxmProgramParameterGetResourceIndex(pos);
    attrs[1].streamIndex = 0;
    attrs[1].offset = 8;
    attrs[1].format = SCE_GXM_ATTRIBUTE_FORMAT_F32;
    attrs[1].componentCount = 2;
    attrs[1].regIndex = (uint16_t)sceGxmProgramParameterGetResourceIndex(uv);
    attrs[2].streamIndex = 0;
    attrs[2].offset = 16;
    attrs[2].format = SCE_GXM_ATTRIBUTE_FORMAT_U8N;
    attrs[2].componentCount = 4;
    attrs[2].regIndex = (uint16_t)sceGxmProgramParameterGetResourceIndex(col);
    SceGxmVertexStream stream;
    stream.stride = sizeof(Vtx);
    stream.indexSource = SCE_GXM_INDEX_SOURCE_INDEX_16BIT;
    if (sceGxmShaderPatcherCreateVertexProgram(g_patcher, vertex_id, attrs, 3, &stream, 1, &g_vertex) < 0) return false;
    for (int p = 0; p < PROG_COUNT; p++) {
        SceGxmShaderPatcherId id;
        const SceGxmProgram* program = nullptr;
        if (!register_program(FRAGMENT_NAMES[p], &id, &program)) return false;
        g_sampler[p] = sceGxmProgramFindParameterByName(program, "uTexture");
        if (p == PROG_GRAY) g_level = sceGxmProgramFindParameterByName(program, "uLevel");
        for (int b = 0; b < BLENDS; b++) {
            SceGxmBlendInfo info = blend_info((Blend)b);
            if (sceGxmShaderPatcherCreateFragmentProgram(g_patcher, id, SCE_GXM_OUTPUT_REGISTER_FORMAT_UCHAR4, SCE_GXM_MULTISAMPLE_NONE, &info,
                                                         g_vertex_program, &g_frag[p][b]) < 0) {
                plat::log("gfx: fragment program %s/%d failed", FRAGMENT_NAMES[p], b);
                return false;
            }
        }
    }
    return g_sampler[PROG_TEXTURE] && g_sampler[PROG_GRAY] && g_level;
}

void begin_scene(Surface& s) {
    sceGxmBeginScene(g_ctx, 0, g_rt, nullptr, nullptr, s.sync, &s.color, nullptr);
    g_target = &s;
    g_in_scene = true;
    sceGxmSetViewport(g_ctx, SCREEN_W * 0.5f, SCREEN_W * 0.5f, SCREEN_H * 0.5f, -SCREEN_H * 0.5f, 0.5f, 0.5f);
    sceGxmSetCullMode(g_ctx, SCE_GXM_CULL_NONE);
    sceGxmSetTwoSidedEnable(g_ctx, SCE_GXM_TWO_SIDED_DISABLED);
    sceGxmSetFrontDepthFunc(g_ctx, SCE_GXM_DEPTH_FUNC_ALWAYS);
    sceGxmSetFrontDepthWriteEnable(g_ctx, SCE_GXM_DEPTH_WRITE_DISABLED);
    sceGxmSetVertexProgram(g_ctx, g_vertex);
}

void* ring_alloc(size_t bytes) {
    size_t at = (g_ring_used + 15) & ~(size_t)15;
    if (at + bytes > RING_BYTES) {
        Surface* s = g_in_scene ? g_target : nullptr;
        if (s) {
            sceGxmEndScene(g_ctx, nullptr, nullptr);
            g_in_scene = false;
        }
        sceGxmFinish(g_ctx);
        at = 0;
        if (s) begin_scene(*s);
    }
    g_ring_used = at + bytes;
    return g_ring + at;
}

void draw_now(Program prog, const Texture* tex, Blend blend, bool smooth, const Vtx* v, int n, bool quads, float level) {
    if (n <= 0) return;
    void* mem = ring_alloc((size_t)n * sizeof(Vtx));
    std::memcpy(mem, v, (size_t)n * sizeof(Vtx));
    sceGxmSetVertexProgram(g_ctx, g_vertex);
    sceGxmSetFragmentProgram(g_ctx, g_frag[prog][(int)blend]);
    void* vub = nullptr;
    sceGxmReserveVertexDefaultUniformBuffer(g_ctx, &vub);
    sceGxmSetUniformDataF(vub, g_transform, 0, 4, TRANSFORM);
    if (prog == PROG_GRAY) {
        void* fub = nullptr;
        sceGxmReserveFragmentDefaultUniformBuffer(g_ctx, &fub);
        sceGxmSetUniformDataF(fub, g_level, 0, 1, &level);
    }
    if (tex && g_sampler[prog]) {
        SceGxmTexture t = tex->gxm;
        SceGxmTextureFilter f = smooth ? SCE_GXM_TEXTURE_FILTER_LINEAR : SCE_GXM_TEXTURE_FILTER_POINT;
        sceGxmTextureSetMinFilter(&t, f);
        sceGxmTextureSetMagFilter(&t, f);
        sceGxmSetFragmentTexture(g_ctx, sceGxmProgramParameterGetResourceIndex(g_sampler[prog]), &t);
    }
    sceGxmSetVertexStream(g_ctx, 0, mem);
    if (quads) sceGxmDraw(g_ctx, SCE_GXM_PRIMITIVE_TRIANGLES, SCE_GXM_INDEX_FORMAT_U16, g_quad_indices, (unsigned)(n / 4 * 6));
    else sceGxmDraw(g_ctx, SCE_GXM_PRIMITIVE_TRIANGLES, SCE_GXM_INDEX_FORMAT_U16, g_seq_indices, (unsigned)n);
}

void flush() {
    if (!g_count) return;
    int n = g_count;
    g_count = 0;
    if (!g_in_scene) return;
    draw_now(g_tex ? PROG_TEXTURE : PROG_COLOR, g_tex, g_blend, g_smooth, g_batch, n, g_quads, 0.0f);
}

void end_scene() {
    if (!g_in_scene) return;
    flush();
    sceGxmEndScene(g_ctx, nullptr, nullptr);
    g_in_scene = false;
}

void collect_garbage() {
    for (Texture* t : g_garbage) {
        if (g_tex == t) g_tex = nullptr;
        gpu_free(t->mem);
        delete t;
    }
    g_garbage.clear();
}

bool reclaim() {
    if (g_garbage.empty() || !g_ctx) return false;
    Surface* s = g_in_scene ? g_target : nullptr;
    end_scene();
    sceGxmFinish(g_ctx);
    collect_garbage();
    if (s) begin_scene(*s);
    return true;
}

void* gpu_alloc(size_t bytes) {
    uint32_t need = (uint32_t)std::max<size_t>((bytes + POOL_UNIT - 1) / POOL_UNIT * POOL_UNIT, POOL_UNIT);
    void* p = gpu_take(need);
    if (!p && reclaim()) p = gpu_take(need);
    if (!p) {
        Pool* extra = pool_add(SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE, std::max<size_t>(EXTRA_BLOCK, need), 4096);
        if (extra) {
            plat::log("gfx: added %u KB of texture memory in main ram", (unsigned)(extra->size >> 10));
            p = pool_take(extra, need);
        }
    }
    return p;
}

bool pow2(int n) { return n >= 8 && (n & (n - 1)) == 0; }

void init_texture(Texture& t, void* mem, int w, int h, int stride) {
    t.w = w;
    t.h = h;
    t.mem = mem;
    if (pow2(w) && pow2(h) && stride == w) sceGxmTextureInitLinear(&t.gxm, mem, SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ABGR, (unsigned)w, (unsigned)h, 0);
    else sceGxmTextureInitLinearStrided(&t.gxm, mem, SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ABGR, (unsigned)w, (unsigned)h, (unsigned)stride * 4);
    sceGxmTextureSetUAddrMode(&t.gxm, SCE_GXM_TEXTURE_ADDR_CLAMP);
    sceGxmTextureSetVAddrMode(&t.gxm, SCE_GXM_TEXTURE_ADDR_CLAMP);
}

bool make_surface(Surface& s, bool display) {
    size_t bytes = (size_t)SCREEN_W * SCREEN_H * 4;
    s.mem = gpu_alloc(bytes);
    if (!s.mem) return false;
    uint32_t* px = (uint32_t*)s.mem;
    for (size_t i = 0; i < bytes / 4; i++) px[i] = 0xFF000000u;
    sceGxmColorSurfaceInit(&s.color, SCE_GXM_COLOR_FORMAT_A8B8G8R8, SCE_GXM_COLOR_SURFACE_LINEAR, SCE_GXM_COLOR_SURFACE_SCALE_NONE,
                           SCE_GXM_OUTPUT_REGISTER_SIZE_32BIT, SCREEN_W, SCREEN_H, SCREEN_W, s.mem);
    if (display && sceGxmSyncObjectCreate(&s.sync) < 0) return false;
    init_texture(s.tex, s.mem, SCREEN_W, SCREEN_H, SCREEN_W);
    return true;
}

void prepare(Texture* tex, Blend blend, bool smooth, bool quads, int verts) {
    if (g_count && (g_tex != tex || g_blend != blend || g_smooth != smooth || g_quads != quads || g_count + verts > MAX_VTX)) flush();
    g_tex = tex;
    g_blend = blend;
    g_smooth = smooth;
    g_quads = quads;
}

bool scissor_full() { return g_sx0 <= 0 && g_sy0 <= 0 && g_sx1 >= SCREEN_W && g_sy1 >= SCREEN_H; }
bool scissor_empty() { return g_sx1 <= g_sx0 || g_sy1 <= g_sy0; }

bool clip_rect(float& x0, float& y0, float& x1, float& y1, float& u0, float& v0, float& u1, float& v1) {
    if (scissor_empty() || x1 <= x0 || y1 <= y0) return false;
    if (scissor_full()) return true;
    float cx0 = std::max(x0, (float)g_sx0), cy0 = std::max(y0, (float)g_sy0);
    float cx1 = std::min(x1, (float)g_sx1), cy1 = std::min(y1, (float)g_sy1);
    if (cx1 <= cx0 || cy1 <= cy0) return false;
    float du = (u1 - u0) / (x1 - x0), dv = (v1 - v0) / (y1 - y0);
    float nu0 = u0 + (cx0 - x0) * du, nu1 = u0 + (cx1 - x0) * du;
    float nv0 = v0 + (cy0 - y0) * dv, nv1 = v0 + (cy1 - y0) * dv;
    x0 = cx0;
    y0 = cy0;
    x1 = cx1;
    y1 = cy1;
    u0 = nu0;
    u1 = nu1;
    v0 = nv0;
    v1 = nv1;
    return true;
}

void push_rect(float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1, uint32_t color) {
    Vtx* v = &g_batch[g_count];
    v[0] = {x0, y0, u0, v0, color};
    v[1] = {x1, y0, u1, v0, color};
    v[2] = {x0, y1, u0, v1, color};
    v[3] = {x1, y1, u1, v1, color};
    g_count += 4;
}

int clip_edge(const Vtx* in, int n, Vtx* out, bool vertical, float limit, bool keep_greater) {
    int m = 0;
    for (int i = 0; i < n; i++) {
        const Vtx& a = in[i];
        const Vtx& b = in[(i + 1) % n];
        float da = (vertical ? a.y : a.x) - limit, db = (vertical ? b.y : b.x) - limit;
        if (!keep_greater) {
            da = -da;
            db = -db;
        }
        if (da >= 0) out[m++] = a;
        if ((da >= 0) != (db >= 0)) {
            float t = da / (da - db);
            out[m++] = {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.u + (b.u - a.u) * t, a.v + (b.v - a.v) * t, a.color};
        }
    }
    return m;
}

void push_triangle(const Vtx& a, const Vtx& b, const Vtx& c) {
    if (scissor_empty()) return;
    Vtx p0[9] = {a, b, c}, p1[9];
    int n = 3;
    if (!scissor_full()) {
        n = clip_edge(p0, n, p1, false, (float)g_sx0, true);
        n = clip_edge(p1, n, p0, false, (float)g_sx1, false);
        n = clip_edge(p0, n, p1, true, (float)g_sy0, true);
        n = clip_edge(p1, n, p0, true, (float)g_sy1, false);
    }
    for (int i = 1; i + 1 < n; i++) {
        if (g_count + 3 > MAX_VTX) flush();
        g_batch[g_count++] = p0[0];
        g_batch[g_count++] = p0[i];
        g_batch[g_count++] = p0[i + 1];
    }
}

float nu(const Texture* t, float u) { return u / (float)t->w; }
float nv(const Texture* t, float v) { return v / (float)t->h; }

}

void reserve(size_t) {}
void* alloc(size_t bytes) { return memalign(64, bytes ? bytes : 16); }
void release(void* p) { std::free(p); }

Texture* texture_create(int w, int h, int stride, int rows, Format fmt, bool tiled, void* pixels, uint32_t* palette) {
    Texture* t = nullptr;
    if (fmt != Format::RGBA8888 || tiled) {
        plat::log("gfx: unsupported texture format %d%s", (int)fmt, tiled ? " tiled" : "");
    } else if (g_ready && pixels && w > 0 && h > 0 && stride >= w && rows >= h) {
        size_t bytes = (size_t)stride * (size_t)rows * 4;
        void* mem = gpu_alloc(bytes);
        if (mem) {
            std::memcpy(mem, pixels, bytes);
            t = new Texture();
            init_texture(*t, mem, w, h, stride);
            t->bytes = bytes;
        } else {
            plat::log("gfx: out of texture memory for %dx%d", w, h);
        }
    }
    release(pixels);
    release(palette);
    return t;
}

void texture_destroy(Texture* t) {
    if (t) g_garbage.push_back(t);
}

size_t texture_bytes(const Texture* t) { return t ? t->bytes : 0; }
int texture_width(const Texture* t) { return t ? t->w : 0; }
int texture_height(const Texture* t) { return t ? t->h : 0; }

void init() {
    SceGxmInitializeParams ip{};
    ip.flags = 0;
    ip.displayQueueMaxPendingCount = 2;
    ip.displayQueueCallback = display_callback;
    ip.displayQueueCallbackDataSize = sizeof(DisplayData);
    ip.parameterBufferSize = SCE_GXM_DEFAULT_PARAMETER_BUFFER_SIZE;
    if (sceGxmInitialize(&ip) < 0) {
        plat::log("gfx: sceGxmInitialize failed");
        return;
    }
    for (size_t mb = 112; mb >= 32; mb -= 4) {
        if (pool_add(SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW, mb << 20, 256 * 1024)) {
            plat::log("gfx: %u MB of video memory", (unsigned)mb);
            break;
        }
    }
    SceGxmContextParams cp{};
    cp.hostMem = std::malloc(SCE_GXM_MINIMUM_CONTEXT_HOST_MEM_SIZE);
    cp.hostMemSize = SCE_GXM_MINIMUM_CONTEXT_HOST_MEM_SIZE;
    cp.vdmRingBufferMem = mapped_block(SCE_GXM_DEFAULT_VDM_RING_BUFFER_SIZE);
    cp.vdmRingBufferMemSize = SCE_GXM_DEFAULT_VDM_RING_BUFFER_SIZE;
    cp.vertexRingBufferMem = mapped_block(SCE_GXM_DEFAULT_VERTEX_RING_BUFFER_SIZE);
    cp.vertexRingBufferMemSize = SCE_GXM_DEFAULT_VERTEX_RING_BUFFER_SIZE;
    cp.fragmentRingBufferMem = mapped_block(SCE_GXM_DEFAULT_FRAGMENT_RING_BUFFER_SIZE);
    cp.fragmentRingBufferMemSize = SCE_GXM_DEFAULT_FRAGMENT_RING_BUFFER_SIZE;
    unsigned int usse_offset = 0;
    cp.fragmentUsseRingBufferMem = usse_block(SCE_GXM_DEFAULT_FRAGMENT_USSE_RING_BUFFER_SIZE, false, &usse_offset);
    cp.fragmentUsseRingBufferMemSize = SCE_GXM_DEFAULT_FRAGMENT_USSE_RING_BUFFER_SIZE;
    cp.fragmentUsseRingBufferOffset = usse_offset;
    if (sceGxmCreateContext(&cp, &g_ctx) < 0) {
        plat::log("gfx: sceGxmCreateContext failed");
        g_ctx = nullptr;
        return;
    }
    const size_t patcher_bytes = 64 * 1024, usse_bytes = 64 * 1024;
    SceGxmShaderPatcherParams pp{};
    pp.hostAllocCallback = patcher_alloc;
    pp.hostFreeCallback = patcher_free;
    pp.bufferMem = mapped_block(patcher_bytes);
    pp.bufferMemSize = patcher_bytes;
    pp.vertexUsseMem = usse_block(usse_bytes, true, &pp.vertexUsseOffset);
    pp.vertexUsseMemSize = usse_bytes;
    pp.fragmentUsseMem = usse_block(usse_bytes, false, &pp.fragmentUsseOffset);
    pp.fragmentUsseMemSize = usse_bytes;
    if (sceGxmShaderPatcherCreate(&pp, &g_patcher) < 0 || !setup_programs()) {
        plat::log("gfx: shader setup failed");
        return;
    }
    g_ring = (uint8_t*)mapped_block(RING_BYTES);
    g_quad_indices = (uint16_t*)mapped_block(MAX_VTX / 4 * 6 * sizeof(uint16_t));
    g_seq_indices = (uint16_t*)mapped_block(MAX_VTX * sizeof(uint16_t));
    if (!g_ring || !g_quad_indices || !g_seq_indices) {
        plat::log("gfx: out of memory for vertex buffers");
        return;
    }
    for (int q = 0; q < MAX_VTX / 4; q++) {
        uint16_t b = (uint16_t)(q * 4);
        uint16_t* d = g_quad_indices + q * 6;
        d[0] = b;
        d[1] = (uint16_t)(b + 1);
        d[2] = (uint16_t)(b + 2);
        d[3] = (uint16_t)(b + 1);
        d[4] = (uint16_t)(b + 3);
        d[5] = (uint16_t)(b + 2);
    }
    for (int i = 0; i < MAX_VTX; i++) g_seq_indices[i] = (uint16_t)i;
    SceGxmRenderTargetParams rp{};
    rp.width = SCREEN_W;
    rp.height = SCREEN_H;
    rp.scenesPerFrame = 8;
    rp.multisampleMode = SCE_GXM_MULTISAMPLE_NONE;
    rp.driverMemBlock = -1;
    if (sceGxmCreateRenderTarget(&rp, &g_rt) < 0 || !make_surface(g_display[0], true) || !make_surface(g_display[1], true) || !make_surface(g_grab, false)) {
        plat::log("gfx: render target setup failed");
        return;
    }
    DisplayData first{g_display[g_front].mem};
    display_callback(&first);
    g_ready = true;
}

void shutdown() {
    if (!g_ready) return;
    end_scene();
    sceGxmDisplayQueueFinish();
    sceGxmFinish(g_ctx);
}

void begin_frame(uint32_t clear_color) {
    if (!g_ready) return;
    end_scene();
    sceGxmFinish(g_ctx);
    collect_garbage();
    g_ring_used = 0;
    g_count = 0;
    g_tex = nullptr;
    begin_scene(g_display[g_back]);
    g_frame_open = true;
    reset_scissor();
    fill(0, 0, SCREEN_W, SCREEN_H, clear_color | 0xFF000000u, Blend::Copy);
}

void end_frame() {
    if (!g_frame_open) return;
    end_scene();
    g_frame_open = false;
    Surface& d = g_display[g_back];
    sceGxmPadHeartbeat(&d.color, d.sync);
    DisplayData data{d.mem};
    sceGxmDisplayQueueAddEntry(g_display[g_front].sync, d.sync, &data);
    g_shown = g_back;
    g_front = g_back;
    g_back ^= 1;
}

const uint32_t* framebuffer() {
    static std::vector<uint32_t> px;
    px.assign((size_t)SCREEN_W * SCREEN_H, 0xFF000000u);
    if (!g_ready) return px.data();
    if (!g_in_scene) sceGxmFinish(g_ctx);
    const uint32_t* src = (const uint32_t*)g_display[g_shown].mem;
    for (size_t i = 0; i < px.size(); i++) px[i] = src[i] | 0xFF000000u;
    return px.data();
}

void set_scissor(int x, int y, int w, int h) {
    g_sx0 = std::clamp(x, 0, SCREEN_W);
    g_sy0 = std::clamp(y, 0, SCREEN_H);
    g_sx1 = std::clamp(x + w, g_sx0, SCREEN_W);
    g_sy1 = std::clamp(y + h, g_sy0, SCREEN_H);
}

void reset_scissor() { set_scissor(0, 0, SCREEN_W, SCREEN_H); }

void sprite(Texture* tex, float u0, float v0, float u1, float v1, float x0, float y0, float x1, float y1, uint32_t color, Blend blend, bool smooth) {
    if (!tex || !g_frame_open) return;
    if (x1 < x0) {
        std::swap(x0, x1);
        std::swap(u0, u1);
    }
    if (y1 < y0) {
        std::swap(y0, y1);
        std::swap(v0, v1);
    }
    if (!clip_rect(x0, y0, x1, y1, u0, v0, u1, v1)) return;
    prepare(tex, blend, smooth, true, 4);
    push_rect(x0, y0, x1, y1, nu(tex, u0), nv(tex, v0), nu(tex, u1), nv(tex, v1), color);
}

void draw_quad(Texture* tex, const float uv[8], const float xy[8], uint32_t color, Blend blend, bool smooth) {
    if (!tex || !g_frame_open) return;
    prepare(tex, blend, smooth, false, 6);
    Vtx c[4];
    for (int k = 0; k < 4; k++) c[k] = {xy[k * 2], xy[k * 2 + 1], nu(tex, uv[k * 2]), nv(tex, uv[k * 2 + 1]), color};
    push_triangle(c[0], c[1], c[2]);
    push_triangle(c[0], c[2], c[3]);
}

void fill(float x0, float y0, float x1, float y1, uint32_t color, Blend blend) {
    if (!g_frame_open) return;
    if (x1 < x0) std::swap(x0, x1);
    if (y1 < y0) std::swap(y0, y1);
    float u0 = 0, v0 = 0, u1 = 0, v1 = 0;
    if (!clip_rect(x0, y0, x1, y1, u0, v0, u1, v1)) return;
    prepare(nullptr, blend, false, true, 4);
    push_rect(x0, y0, x1, y1, 0, 0, 0, 0, color);
}

void desaturate(float amount) {
    if (!g_frame_open || amount <= 0) return;
    if (amount > 1) amount = 1;
    Surface& screen = *g_target;
    end_scene();
    const Vtx q[4] = {{0, 0, 0, 0, WHITE}, {SCREEN_W, 0, 1, 0, WHITE}, {0, SCREEN_H, 0, 1, WHITE}, {SCREEN_W, SCREEN_H, 1, 1, WHITE}};
    begin_scene(g_grab);
    draw_now(PROG_GRAY, &screen.tex, Blend::Copy, false, q, 4, true, amount);
    end_scene();
    begin_scene(screen);
    draw_now(PROG_TEXTURE, &g_grab.tex, Blend::Copy, false, q, 4, true, 0.0f);
}

}
