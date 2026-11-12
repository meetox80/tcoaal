#include "../../core/image.h"

#include <psxgpu.h>
#include <stdlib.h>
#include <string.h>

#include <string>
#include <unordered_map>

#include "../platform.h"
#include "disc.h"
#include "texture.h"
#include "vram.h"

Image::~Image() {
    for (ImageChunk& c : chunks) gfx::texture_destroy(c.tex);
}

namespace images {

namespace {

constexpr uint32_t HEADER = 24;

struct Entry {
    Image* img = nullptr;
    uint32_t last = 0;
    bool pinned = false;
    bool missing = false;
};

using Slot = std::pair<const std::string, Entry>;

std::unordered_map<std::string, Entry> g_cache;
std::unordered_map<std::string, Slot*> g_names;
uint32_t g_tick;
size_t g_used;

uint16_t rd16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | p[1] << 8); }

void drop(Entry& e) {
    g_used -= e.img->bytes;
    delete e.img;
    e.img = nullptr;
}

bool evict_one() {
    Entry* victim = nullptr;
    for (Slot& s : g_cache) {
        Entry& e = s.second;
        if (!e.img || e.pinned || e.last + 2 > g_tick) continue;
        if (!victim || e.last < victim->last) victim = &e;
    }
    if (!victim) return false;
    DrawSync(0);
    drop(*victim);
    return true;
}

bool place(int words, int rows, int per, RECT& out) {
    while (!vram::alloc(words, rows, per, out)) {
        if (!evict_one()) return false;
    }
    return true;
}

bool palette(int entries, RECT& out) {
    while (!vram::alloc_clut(entries, out)) {
        if (!evict_one()) return false;
    }
    return true;
}

Image* upload(const uint8_t* d, uint32_t size) {
    int depth = d[4], flags = d[5], count = rd16(d + 6);
    int entries = depth == 0 ? 16 : depth == 1 ? 256 : 0, per = 4 >> depth;
    if (depth > 2 || HEADER + entries * 2u + count * 8u > size) return nullptr;
    Image* img = new Image();
    img->w = rd16(d + 16);
    img->h = rd16(d + 18);
    img->ow = rd16(d + 20);
    img->oh = rd16(d + 22);
    img->fx = img->ow ? static_cast<float>(img->w) / img->ow : 1.0f;
    img->fy = img->oh ? static_cast<float>(img->h) / img->oh : 1.0f;
    img->qx = img->ow ? (img->w << 16) / img->ow : 1 << 16;
    img->qy = img->oh ? (img->h << 16) / img->oh : 1 << 16;
    int ox = rd16(d + 12), oy = rd16(d + 14);
    bool semi = flags & 2;
    const uint8_t* p = d + HEADER;
    RECT pal = {}, fade = {};
    uint16_t clut = 0, fade_clut = 0;
    if (entries) {
        if (!palette(entries, pal)) {
            delete img;
            return nullptr;
        }
        vram::upload(pal, p);
        clut = fade_clut = getClut(pal.x, pal.y);
        if (semi) {
            static uint16_t blended[256];
            for (int i = 0; i < entries; i++) {
                uint16_t c = rd16(p + i * 2);
                blended[i] = c ? static_cast<uint16_t>((c & 0x8000 ? (c >> 1) & 0x3DEF : c) | 0x8000) : 0;
            }
            if (palette(entries, fade)) {
                vram::upload(fade, blended);
                fade_clut = getClut(fade.x, fade.y);
            }
        }
        p += entries * 2;
    }
    const uint8_t* table = p;
    p += count * 8;
    img->chunks.reserve(count);
    for (int i = 0; i < count; i++, table += 8) {
        int w = rd16(table + 4), h = rd16(table + 6), words = (w + per - 1) / per;
        uint32_t bytes = static_cast<uint32_t>((words * h + 1) & ~1) * 2;
        RECT area;
        if (p + bytes > d + size || !place(words, h, per, area)) {
            if (img->chunks.empty()) {
                if (pal.w) vram::release_clut(pal);
                if (fade.w) vram::release_clut(fade);
            }
            delete img;
            return nullptr;
        }
        vram::upload(area, p);
        p += bytes;
        gfx::Texture* t = new gfx::Texture{area,
                                           i ? RECT{} : pal,
                                           i ? RECT{} : fade,
                                           static_cast<uint16_t>(getTPage(depth, 0, area.x, area.y)),
                                           clut,
                                           fade_clut,
                                           static_cast<uint8_t>((area.x & 63) * per),
                                           static_cast<uint8_t>(area.y & 255),
                                           static_cast<uint16_t>(w),
                                           static_cast<uint16_t>(h),
                                           semi};
        img->chunks.push_back({ox + rd16(table), oy + rd16(table + 2), w, h, t});
        img->bytes += gfx::texture_bytes(t);
    }
    if (img->chunks.empty()) {
        if (pal.w) vram::release_clut(pal);
        if (fade.w) vram::release_clut(fade);
    }
    return img;
}

}

Image* load_data(const std::string& rel) {
    const disc::Entry* e = disc::find(rel.c_str());
    if (!e || e->size < HEADER) return nullptr;
    auto* raw = static_cast<uint8_t*>(malloc(disc::sectors(e->size) * disc::SECTOR));
    if (!raw) return nullptr;
    Image* img = disc::read(*e, raw) && !memcmp(raw, "PTEX", 4) ? upload(raw, e->size) : nullptr;
    DrawSync(0);
    free(raw);
    return img;
}

namespace {

Slot& slot_of(const std::string& rel) { return *g_cache.try_emplace(rel).first; }

Image* use(Slot& s) {
    Entry& e = s.second;
    if (!e.img && !e.missing) {
        e.img = load_data(s.first + ".tex");
        e.missing = !e.img;
        if (e.missing) plat::log("missing image %s", s.first.c_str());
        else g_used += e.img->bytes;
    }
    e.last = g_tick;
    return e.img;
}

Slot& named(const char* folder, const std::string& name) {
    static std::string key;
    key.assign(folder).append(1, '/').append(name);
    auto it = g_names.find(key);
    if (it == g_names.end()) it = g_names.emplace(key, &slot_of(resolve(folder, name, ".png"))).first;
    return *it->second;
}

}

Image* get_path(const std::string& rel) { return use(slot_of(rel)); }

Image* get(const char* folder, const std::string& name) { return name.empty() ? nullptr : use(named(folder, name)); }

void preload(const char* folder, const std::string& name) { get(folder, name); }

void prefetch(const char*, const std::string&) {}

void prefetch_path(const std::string&) {}

void set_budget(size_t) {}

void trim() {}

void tick() { g_tick++; }

void clear() {
    DrawSync(0);
    for (Slot& s : g_cache) {
        if (s.second.img && !s.second.pinned) drop(s.second);
    }
}

size_t used_bytes() { return g_used; }

void pin(const char* folder, const std::string& name, bool pinned) {
    if (name.empty()) return;
    Slot& s = named(folder, name);
    use(s);
    s.second.pinned = pinned;
}

}
