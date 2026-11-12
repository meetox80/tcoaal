#include "pack.h"

#include <stdlib.h>
#include <string.h>

PackEntry* pack_add(Pack* p, const char* name) {
    if (p->count == p->cap) {
        p->cap = p->cap ? p->cap * 2 : 256;
        p->items = xrealloc(p->items, sizeof(PackEntry) * (size_t)p->cap);
    }
    PackEntry* e = &p->items[p->count++];
    memset(e, 0, sizeof(*e));
    e->name = xstrdup(name);
    return e;
}

static int cmp_entry(const void* a, const void* b) { return strcmp(((const PackEntry*)a)->name, ((const PackEntry*)b)->name); }

static void put32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static size_t header_size(const Pack* p) {
    size_t names = 0;
    for (int i = 0; i < p->count; i++) names += strlen(p->items[i].name) + 1;
    return 16 + (size_t)p->count * 12 + names;
}

static size_t pad_to(uint64_t pos) { return (size_t)((64 - pos % 64) % 64); }

uint64_t pack_size(Pack* p) {
    qsort(p->items, (size_t)p->count, sizeof(PackEntry), cmp_entry);
    uint64_t pos = header_size(p);
    for (int i = 0; i < p->count; i++) pos += pad_to(pos) + p->items[i].data.len;
    return pos;
}

void pack_stream(const void* pack, Emit emit, void* ctx) {
    const Pack* p = pack;
    static const uint8_t zero[64];
    size_t head = header_size(p);
    uint8_t* h = xcalloc(head, 1);
    memcpy(h, "TPAK", 4);
    put32(h + 4, PACK_VERSION);
    put32(h + 8, (uint32_t)p->count);
    put32(h + 12, (uint32_t)(head - 16 - (size_t)p->count * 12));
    char* name_blob = (char*)h + 16 + (size_t)p->count * 12;
    size_t name_off = 0;
    uint64_t pos = head;
    for (int i = 0; i < p->count; i++) {
        size_t n = strlen(p->items[i].name) + 1;
        memcpy(name_blob + name_off, p->items[i].name, n);
        uint8_t* e = h + 16 + (size_t)i * 12;
        put32(e, (uint32_t)name_off);
        name_off += n;
        pos += pad_to(pos);
        put32(e + 4, (uint32_t)pos);
        put32(e + 8, (uint32_t)p->items[i].data.len);
        pos += p->items[i].data.len;
    }
    emit(ctx, h, head);
    free(h);
    pos = head;
    for (int i = 0; i < p->count; i++) {
        size_t pad = pad_to(pos);
        emit(ctx, zero, pad);
        emit(ctx, p->items[i].data.data, p->items[i].data.len);
        pos += pad + p->items[i].data.len;
    }
}

static void buf_emit(void* ctx, const void* data, size_t len) { buf_put((Buf*)ctx, data, len); }

static void file_emit(void* ctx, const void* data, size_t len) {
    FILE** f = ctx;
    if (*f && len && fwrite(data, 1, len, *f) != len) {
        fclose(*f);
        *f = NULL;
    }
}

void pack_build(Pack* p, Buf* out) {
    pack_size(p);
    pack_stream(p, buf_emit, out);
}

int pack_write(Pack* p, const char* path) {
    pack_size(p);
    FILE* f = file_open(path, "wb");
    if (!f) return 0;
    pack_stream(p, file_emit, &f);
    return f && fclose(f) == 0;
}

void pack_free(Pack* p) {
    for (int i = 0; i < p->count; i++) {
        free(p->items[i].name);
        free(p->items[i].error);
        buf_free(&p->items[i].data);
    }
    free(p->items);
    memset(p, 0, sizeof(*p));
}

uint32_t pack_hash(const char* name) {
    uint32_t h = 2166136261u;
    for (const unsigned char* p = (const unsigned char*)name; *p; p++) h = (h ^ *p) * 16777619u;
    return h;
}

typedef struct {
    uint32_t hash;
    uint32_t sector;
    uint32_t size;
    const char* name;
} DiscEntry;

static int cmp_hash(const void* a, const void* b) {
    uint32_t x = ((const DiscEntry*)a)->hash, y = ((const DiscEntry*)b)->hash;
    return (x > y) - (x < y);
}

void pack_build_disc(Pack* p, Buf* out) {
    qsort(p->items, (size_t)p->count, sizeof(PackEntry), cmp_entry);
    DiscEntry* idx = xmalloc(sizeof(DiscEntry) * (size_t)(p->count + 1));
    uint32_t head = (uint32_t)((16 + (size_t)p->count * 12 + PACK_SECTOR - 1) / PACK_SECTOR);
    uint32_t sector = head;
    for (int i = 0; i < p->count; i++) {
        uint32_t len = (uint32_t)p->items[i].data.len;
        idx[i] = (DiscEntry){pack_hash(p->items[i].name), sector, len, p->items[i].name};
        sector += (len + PACK_SECTOR - 1) / PACK_SECTOR;
    }
    qsort(idx, (size_t)p->count, sizeof(DiscEntry), cmp_hash);
    for (int i = 1; i < p->count; i++) {
        if (idx[i].hash == idx[i - 1].hash) fatal("pack: %s and %s share a hash", idx[i - 1].name, idx[i].name);
    }
    size_t base = out->len;
    buf_reserve(out, (size_t)sector * PACK_SECTOR);
    buf_zero(out, (size_t)head * PACK_SECTOR);
    uint8_t* h = out->data + base;
    memcpy(h, "TPAK", 4);
    put32(h + 4, PACK_DISC_VERSION);
    put32(h + 8, (uint32_t)p->count);
    put32(h + 12, sector);
    for (int i = 0; i < p->count; i++) {
        uint8_t* e = h + 16 + (size_t)i * 12;
        put32(e, idx[i].hash);
        put32(e + 4, idx[i].sector);
        put32(e + 8, idx[i].size);
    }
    for (int i = 0; i < p->count; i++) {
        const Buf* d = &p->items[i].data;
        buf_put(out, d->data, d->len);
        buf_zero(out, (PACK_SECTOR - d->len % PACK_SECTOR) % PACK_SECTOR);
    }
    free(idx);
}

static uint32_t get32(const uint8_t* p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

int pbp_read(const uint8_t* data, size_t len, Pbp* out) {
    memset(out, 0, sizeof(*out));
    if (len < 40 || memcmp(data, "\0PBP", 4) != 0) return 0;
    uint32_t off[9];
    for (int i = 0; i < 8; i++) off[i] = get32(data + 8 + i * 4);
    off[8] = (uint32_t)len;
    for (int i = 0; i < 8; i++) {
        if (off[i] > off[i + 1] || off[i + 1] > len) return 0;
        buf_put(&out->part[i], data + off[i], off[i + 1] - off[i]);
    }
    return 1;
}

void pbp_write(const Pbp* p, Buf* out) {
    uint32_t off = 40;
    buf_put(out, "\0PBP", 4);
    buf_u32(out, 0x00010000);
    for (int i = 0; i < 8; i++) {
        buf_u32(out, off);
        off += (uint32_t)p->part[i].len;
    }
    for (int i = 0; i < 8; i++) buf_put(out, p->part[i].data, p->part[i].len);
}

void pbp_free(Pbp* p) {
    for (int i = 0; i < 8; i++) buf_free(&p->part[i]);
}

typedef struct {
    const char* key;
    int is_int;
    uint32_t ival;
    const char* sval;
} SfoEntry;

void sfo_build(const char* title, Buf* out) {
    SfoEntry e[] = {
        {"BOOTABLE", 1, 1, NULL},      {"CATEGORY", 0, 0, "MG"},          {"DISC_ID", 0, 0, "TCOAAL001"},
        {"DISC_VERSION", 0, 0, "1.00"}, {"MEMSIZE", 1, 1, NULL},           {"PARENTAL_LEVEL", 1, 1, NULL},
        {"PSP_SYSTEM_VER", 0, 0, "1.00"}, {"REGION", 1, 32768, NULL},       {"TITLE", 0, 0, title},
    };
    int n = (int)(sizeof(e) / sizeof(e[0]));
    Buf keys = {0}, vals = {0}, index = {0};
    for (int i = 0; i < n; i++) {
        uint32_t key_off = (uint32_t)keys.len;
        buf_put(&keys, e[i].key, strlen(e[i].key) + 1);
        uint32_t data_off = (uint32_t)vals.len;
        uint16_t fmt;
        uint32_t length, maxlen;
        if (e[i].is_int) {
            buf_u32(&vals, e[i].ival);
            fmt = 0x0404;
            length = maxlen = 4;
        } else {
            length = (uint32_t)strlen(e[i].sval) + 1;
            maxlen = (length + 3) / 4 * 4;
            if (!strcmp(e[i].key, "TITLE")) maxlen = 128;
            if (!strcmp(e[i].key, "DISC_ID")) maxlen = 16;
            if (!strcmp(e[i].key, "CATEGORY")) maxlen = 4;
            if (!strcmp(e[i].key, "DISC_VERSION") || !strcmp(e[i].key, "PSP_SYSTEM_VER")) maxlen = 8;
            buf_put(&vals, e[i].sval, length);
            buf_zero(&vals, maxlen - length);
            fmt = 0x0204;
        }
        buf_u16(&index, (uint16_t)key_off);
        buf_u16(&index, fmt);
        buf_u32(&index, length);
        buf_u32(&index, maxlen);
        buf_u32(&index, data_off);
    }
    while (keys.len % 4) buf_u8(&keys, 0);
    uint32_t key_start = 20 + (uint32_t)index.len;
    uint32_t data_start = key_start + (uint32_t)keys.len;
    buf_put(out, "\0PSF", 4);
    buf_u32(out, 0x00000101);
    buf_u32(out, key_start);
    buf_u32(out, data_start);
    buf_u32(out, (uint32_t)n);
    buf_put(out, index.data, index.len);
    buf_put(out, keys.data, keys.len);
    buf_put(out, vals.data, vals.len);
    buf_free(&keys);
    buf_free(&vals);
    buf_free(&index);
}

static uint64_t get64(const uint8_t* p) { return (uint64_t)get32(p) | (uint64_t)get32(p + 4) << 32; }

#define CTR_HEADER_SIZE 0x2C
#define CTR_SMDH_SIZE 0x36C0
#define CTR_ICON_OFFSET 0x2040
#define ROMFS_NONE 0xFFFFFFFFu

int ctr_check(const uint8_t* data, size_t len) {
    if (len < CTR_HEADER_SIZE || memcmp(data, "3DSX", 4) != 0) return 0;
    if ((data[4] | data[5] << 8) < CTR_HEADER_SIZE) return 0;
    uint32_t smdh = get32(data + 0x20), smdh_size = get32(data + 0x24), fs = get32(data + 0x28);
    if (smdh_size != CTR_SMDH_SIZE || (size_t)smdh + smdh_size > len) return 0;
    return fs <= len;
}

static uint32_t romfs_hash(uint32_t parent, const char* name, uint32_t buckets) {
    uint32_t h = parent ^ 123456789u;
    for (const char* c = name; *c; c++) {
        h = (h >> 5) | (h << 27);
        h ^= (uint8_t)*c;
    }
    return h % buckets;
}

static void romfs_build(const char* name, const Buf* data, Buf* out) {
    uint32_t name_bytes = (uint32_t)strlen(name) * 2;
    uint32_t entry = (0x20 + name_bytes + 3) & ~3u;
    uint32_t dir_hash = 0x28, dir_table = dir_hash + 12, file_hash = dir_table + 0x18, file_table = file_hash + 12;
    uint32_t file_data = file_table + entry;
    uint32_t head[10] = {0x28, dir_hash, 12, dir_table, 0x18, file_hash, 12, file_table, entry, file_data};
    for (int i = 0; i < 10; i++) buf_u32(out, head[i]);
    for (uint32_t i = 0; i < 3; i++) buf_u32(out, i == romfs_hash(0, "", 3) ? 0 : ROMFS_NONE);
    uint32_t root[6] = {0, ROMFS_NONE, ROMFS_NONE, 0, ROMFS_NONE, 0};
    for (int i = 0; i < 6; i++) buf_u32(out, root[i]);
    for (uint32_t i = 0; i < 3; i++) buf_u32(out, i == romfs_hash(0, name, 3) ? 0 : ROMFS_NONE);
    buf_u32(out, 0);
    buf_u32(out, ROMFS_NONE);
    buf_u32(out, 0);
    buf_u32(out, 0);
    buf_u32(out, (uint32_t)data->len);
    buf_u32(out, (uint32_t)((uint64_t)data->len >> 32));
    buf_u32(out, ROMFS_NONE);
    buf_u32(out, name_bytes);
    for (const char* c = name; *c; c++) buf_u16(out, (uint8_t)*c);
    buf_zero(out, entry - 0x20 - name_bytes);
    buf_put(out, data->data, data->len);
    buf_zero(out, (4 - data->len % 4) % 4);
}

static int romfs_find(const uint8_t* r, size_t len, const char* name, size_t* off, size_t* size) {
    if (len < 0x28) return 0;
    uint32_t table = get32(r + 0x1C), table_size = get32(r + 0x20), data = get32(r + 0x24);
    if ((size_t)table + table_size > len) return 0;
    size_t nlen = strlen(name);
    for (uint32_t pos = 0; pos + 0x20 <= table_size;) {
        const uint8_t* e = r + table + pos;
        uint64_t doff = get64(e + 8), dsize = get64(e + 16);
        uint32_t name_len = get32(e + 0x1C);
        if ((size_t)pos + 0x20 + name_len > table_size) return 0;
        int match = name_len == nlen * 2;
        for (size_t i = 0; match && i < nlen; i++) match = e[0x20 + i * 2] == (uint8_t)name[i] && e[0x21 + i * 2] == 0;
        if (match) {
            if ((uint64_t)data + doff + dsize > len) return 0;
            *off = (size_t)(data + doff);
            *size = (size_t)dsize;
            return 1;
        }
        pos += (0x20 + name_len + 3) & ~3u;
    }
    return 0;
}

int ctr_build(const uint8_t* engine, size_t engine_len, const uint8_t* icon, const Buf* pak, Buf* out) {
    if (!ctr_check(engine, engine_len)) return 0;
    uint32_t fs = get32(engine + 0x28);
    size_t code = fs ? fs : engine_len;
    size_t base = out->len;
    buf_put(out, engine, code);
    uint8_t* h = out->data + base;
    if (icon) memcpy(h + get32(h + 0x20) + CTR_ICON_OFFSET, icon, CTR_ICON_SIZE);
    put32(h + 0x28, (uint32_t)code);
    romfs_build(CTR_PAK_NAME, pak, out);
    return 1;
}

int ctr_parts(const uint8_t* data, size_t len, size_t* smdh, size_t* romfs) {
    if (!ctr_check(data, len) || !get32(data + 0x28)) return 0;
    *smdh = get32(data + 0x20);
    *romfs = get32(data + 0x28);
    return 1;
}

int ctr_unpack(const uint8_t* data, size_t len, Buf* icon, Buf* pak) {
    if (!ctr_check(data, len)) return 0;
    uint32_t fs = get32(data + 0x28);
    size_t off, size;
    if (!fs || !romfs_find(data + fs, len - fs, CTR_PAK_NAME, &off, &size)) return 0;
    buf_put(icon, data + get32(data + 0x20) + CTR_ICON_OFFSET, CTR_ICON_SIZE);
    buf_put(pak, data + fs + off, size);
    return 1;
}

int ctr_pak_version(const uint8_t* data, size_t len) {
    if (!ctr_check(data, len)) return -1;
    uint32_t fs = get32(data + 0x28);
    size_t poff, psize;
    if (!fs || !romfs_find(data + fs, len - fs, CTR_PAK_NAME, &poff, &psize) || psize < 8) return -1;
    const uint8_t* pak = data + fs + poff;
    return memcmp(pak, "TPAK", 4) == 0 ? (int)get32(pak + 4) : -1;
}

int ctr_pak_entry(const uint8_t* data, size_t len, const char* name, size_t* off, size_t* size) {
    if (!ctr_check(data, len)) return 0;
    uint32_t fs = get32(data + 0x28);
    size_t poff, psize;
    if (!fs || !romfs_find(data + fs, len - fs, CTR_PAK_NAME, &poff, &psize) || psize < 16) return 0;
    const uint8_t* pak = data + fs + poff;
    uint32_t count = get32(pak + 8), names = get32(pak + 12);
    size_t blob = 16 + (size_t)count * 12;
    if (memcmp(pak, "TPAK", 4) != 0 || blob + names > psize) return 0;
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t* e = pak + 16 + (size_t)i * 12;
        uint32_t noff = get32(e), doff = get32(e + 4), dsize = get32(e + 8);
        if (noff >= names || strncmp((const char*)pak + blob + noff, name, names - noff) != 0) continue;
        if ((size_t)doff + dsize > psize) return 0;
        *off = fs + poff + doff;
        *size = dsize;
        return 1;
    }
    return 0;
}

static const uint8_t BAYER[16] = {0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5};

static uint16_t dither4(uint8_t v, int x, int y) {
    int q = (v * 32 + (2 * BAYER[(y & 3) * 4 + (x & 3)] + 1) * 17) / 544;
    return (uint16_t)(q > 15 ? 15 : q);
}

static void encode_tiled(const uint8_t* rgba, int w, int h, int rgba4444, Buf* out) {
    for (int ty = 0; ty < h; ty += 8) {
        for (int tx = 0; tx < w; tx += 8) {
            for (int i = 0; i < 64; i++) {
                int x = tx + ((i & 1) | ((i >> 1) & 2) | ((i >> 2) & 4));
                int y = ty + (((i >> 1) & 1) | ((i >> 2) & 2) | ((i >> 3) & 4));
                const uint8_t* p = rgba + ((size_t)y * w + x) * 4;
                if (rgba4444) buf_u16(out, (uint16_t)(dither4(p[0], x, y) << 12 | dither4(p[1], x, y) << 8 | dither4(p[2], x, y) << 4 | p[3] >> 4));
                else buf_u16(out, (uint16_t)((p[0] >> 3) << 11 | (p[1] >> 2) << 5 | p[2] >> 3));
            }
        }
    }
}

void ctr_icon_encode(const uint8_t* rgba, int size, Buf* out) { encode_tiled(rgba, size, size, 0, out); }

void ctr_banner_encode(const uint8_t* rgba, Buf* out) { encode_tiled(rgba, CTR_BANNER_W, CTR_BANNER_H, 1, out); }
