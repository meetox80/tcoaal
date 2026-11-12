#include "pkg.h"

#include <stdlib.h>
#include <string.h>

#define PKG_MAGIC 0x7F504B47u
#define FLAG_SELF 0x80000001u
#define FLAG_RAW 0x80000003u
#define FLAG_DIR 0x80000004u
#define EBOOT_NAME "USRDIR/EBOOT.BIN"
#define CHUNK (8u << 20)

static const char* LICENSE = "Unofficial port of The Coffin of Andy and Leyley, built from the owner's own copy of the game. "
                             "The game is (c) Kit9 Studio / Nemlei. Not affiliated with or endorsed by them or by Sony.";

typedef struct {
    uint32_t h[5];
    uint64_t len;
    uint8_t buf[64];
    size_t fill;
} Sha1;

static const uint32_t SHA1_INIT[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};

static uint32_t rol(uint32_t v, int n) { return (v << n) | (v >> (32 - n)); }

static uint32_t rd32(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static uint64_t rd64(const uint8_t* p) { return (uint64_t)rd32(p) << 32 | rd32(p + 4); }

static void wr32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static void wr64(uint8_t* p, uint64_t v) {
    wr32(p, (uint32_t)(v >> 32));
    wr32(p + 4, (uint32_t)v);
}

static void sha1_block(uint32_t h[5], const uint8_t* p) {
    uint32_t w[80];
    for (int i = 0; i < 16; i++) w[i] = rd32(p + i * 4);
    for (int i = 16; i < 80; i++) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20) {
            f = (b & c) | (~b & d);
            k = 0x5A827999;
        } else if (i < 40) {
            f = b ^ c ^ d;
            k = 0x6ED9EBA1;
        } else if (i < 60) {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8F1BBCDC;
        } else {
            f = b ^ c ^ d;
            k = 0xCA62C1D6;
        }
        uint32_t t = rol(a, 5) + f + e + k + w[i];
        e = d;
        d = c;
        c = rol(b, 30);
        b = a;
        a = t;
    }
    h[0] += a;
    h[1] += b;
    h[2] += c;
    h[3] += d;
    h[4] += e;
}

static void sha1_init(Sha1* s) {
    memcpy(s->h, SHA1_INIT, sizeof(SHA1_INIT));
    s->len = 0;
    s->fill = 0;
}

static void sha1_update(Sha1* s, const void* data, size_t len) {
    const uint8_t* p = data;
    s->len += len;
    if (s->fill) {
        size_t n = 64 - s->fill < len ? 64 - s->fill : len;
        memcpy(s->buf + s->fill, p, n);
        s->fill += n;
        p += n;
        len -= n;
        if (s->fill < 64) return;
        sha1_block(s->h, s->buf);
        s->fill = 0;
    }
    for (; len >= 64; p += 64, len -= 64) sha1_block(s->h, p);
    memcpy(s->buf, p, len);
    s->fill = len;
}

static void sha1_final(Sha1* s, uint8_t out[20]) {
    uint64_t bits = s->len * 8;
    uint8_t pad = 0x80;
    sha1_update(s, &pad, 1);
    uint8_t zero = 0;
    while (s->fill != 56) sha1_update(s, &zero, 1);
    uint8_t lenb[8];
    wr64(lenb, bits);
    sha1_update(s, lenb, 8);
    for (int i = 0; i < 5; i++) wr32(out + i * 4, s->h[i]);
}

void sha1(const void* data, size_t len, uint8_t out[20]) {
    Sha1 s;
    sha1_init(&s);
    sha1_update(&s, data, len);
    sha1_final(&s, out);
}

static void key_context(const uint8_t key[16], uint8_t ctx[64]) {
    memcpy(ctx, key, 8);
    memcpy(ctx + 8, key, 8);
    memcpy(ctx + 16, key + 8, 8);
    memcpy(ctx + 24, key + 8, 8);
    memset(ctx + 32, 0, 32);
}

static void crypt_blocks(const uint8_t base[64], uint64_t counter, uint8_t* data, size_t len) {
    static const uint8_t tail[64] = {0x80, [62] = 0x02};
    uint8_t ctx[64];
    memcpy(ctx, base, 64);
    for (size_t off = 0; off < len; off += 16, counter++) {
        wr64(ctx + 56, counter);
        uint32_t h[5];
        memcpy(h, SHA1_INIT, sizeof(h));
        sha1_block(h, ctx);
        sha1_block(h, tail);
        uint8_t ks[20];
        for (int i = 0; i < 5; i++) wr32(ks + i * 4, h[i]);
        size_t n = len - off < 16 ? len - off : 16;
        for (size_t i = 0; i < n; i++) data[off + i] ^= ks[i];
    }
}

typedef struct {
    uint8_t* data;
    size_t len;
    const uint8_t* ctx;
    uint64_t counter;
    int parts;
} CryptJob;

static void crypt_job(void* p, int index) {
    CryptJob* j = p;
    size_t blocks = (j->len + 15) / 16;
    size_t per = (blocks + (size_t)j->parts - 1) / (size_t)j->parts;
    size_t b0 = per * (size_t)index, b1 = b0 + per < blocks ? b0 + per : blocks;
    if (b0 >= b1) return;
    size_t end = b1 * 16 < j->len ? b1 * 16 : j->len;
    crypt_blocks(j->ctx, j->counter + b0, j->data + b0 * 16, end - b0 * 16);
}

static void crypt_parallel(const uint8_t ctx[64], uint64_t counter, uint8_t* data, size_t len, int threads) {
    if (threads < 1) threads = 1;
    if (len < 65536) threads = 1;
    CryptJob j = {data, len, ctx, counter, threads};
    run_jobs(crypt_job, &j, threads, threads);
}

static uint64_t align16(uint64_t v) { return (v + 15) & ~(uint64_t)15; }

static size_t self_digest(const uint8_t* d, size_t len) {
    static const uint8_t magic[9] = {'S', 'C', 'E', 0, 0, 0, 0, 2, 0x80};
    if (len < 0x68 || memcmp(d, magic, 9) != 0) return 0;
    uint64_t app = rd64(d + 40), doff = rd64(d + 88);
    uint32_t type = app + 0x18 <= len ? rd32(d + app + 12) : 0;
    int found = 0;
    while (doff + 16 <= len) {
        uint32_t dtype = rd32(d + doff), dsize = rd32(d + doff + 4);
        uint64_t next = rd64(d + doff + 8);
        if (dtype == 3) {
            found = 1;
            break;
        }
        if (!dsize) break;
        doff += dsize;
        if (next != 1) break;
    }
    doff += 16;
    if (type != 8 || !found || doff + 0x80 > len) return 0;
    return (size_t)doff;
}

int self_valid(const uint8_t* data, size_t len) {
    if (len < 0x68 || memcmp(data, "SCE\0\0\0\0\2", 8) != 0) return 0;
    uint64_t app = rd64(data + 40);
    return app + 0x18 <= len && rd32(data + app + 12) == 8;
}

typedef struct {
    const char* key;
    int is_int;
    uint32_t ival;
    const char* sval;
} SfoEntry;

void ps3_sfo_build(const char* title, const char* title_id, Buf* out) {
    SfoEntry e[] = {
        {"APP_VER", 0, 0, "01.00"},       {"ATTRIBUTE", 1, 0, NULL},    {"BOOTABLE", 1, 1, NULL},        {"CATEGORY", 0, 0, "HG"},
        {"LICENSE", 0, 0, LICENSE},       {"PARENTAL_LEVEL", 1, 0, NULL}, {"PS3_SYSTEM_VER", 0, 0, "01.8000"}, {"RESOLUTION", 1, 63, NULL},
        {"SOUND_FORMAT", 1, 279, NULL},   {"TITLE", 0, 0, title},       {"TITLE_ID", 0, 0, title_id},    {"VERSION", 0, 0, "01.00"},
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
            uint32_t align = !strcmp(e[i].key, "TITLE") ? 0x80 : !strcmp(e[i].key, "LICENSE") ? 0x200 : !strcmp(e[i].key, "TITLE_ID") ? 0x10 : 4;
            length = (uint32_t)strlen(e[i].sval) + 1;
            maxlen = (length + align - 1) / align * align;
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

typedef struct {
    FILE* f;
    uint8_t ctx[64];
    uint64_t counter;
    uint8_t* chunk;
    size_t fill;
    uint64_t total;
    uint64_t expected;
    uint64_t flushes;
    int threads;
    int ok;
} Writer;

static void writer_flush(Writer* w) {
    if (!w->fill) return;
    crypt_parallel(w->ctx, w->counter, w->chunk, w->fill, w->threads);
    if (fwrite(w->chunk, 1, w->fill, w->f) != w->fill) w->ok = 0;
    w->counter += w->fill / 16;
    w->fill = 0;
    uint64_t chunks = (w->expected + CHUNK - 1) / CHUNK;
    w->flushes++;
    if (chunks >= 8 && (w->flushes % 4 == 0 || w->flushes == chunks)) log_msg("  progress %d/%d", (int)w->flushes, (int)chunks);
}

static void writer_emit(void* ctx, const void* data, size_t len) {
    Writer* w = ctx;
    const uint8_t* p = data;
    w->total += len;
    while (len) {
        size_t n = CHUNK - w->fill < len ? CHUNK - w->fill : len;
        memcpy(w->chunk + w->fill, p, n);
        w->fill += n;
        p += n;
        len -= n;
        if (w->fill == CHUNK) writer_flush(w);
    }
}

static void emit_zeros(Emit emit, void* ctx, uint64_t n) {
    static const uint8_t zero[4096];
    while (n) {
        size_t k = n < sizeof(zero) ? (size_t)n : sizeof(zero);
        emit(ctx, zero, k);
        n -= k;
    }
}

static void emit_item(const PkgItem* it, Emit emit, void* ctx) {
    if (it->data) emit(ctx, it->data->data, it->data->len);
    else if (it->source) it->source(it->source_ctx, emit, ctx);
}

static void sha_emit(void* ctx, const void* data, size_t len) { sha1_update(ctx, data, len); }

static void emit_self(const Buf* b, size_t digest, const char* content_id, Emit emit, void* ctx) {
    uint8_t hash[20], meta[0x80];
    sha1(b->data, b->len, hash);
    memset(meta, 0, sizeof(meta));
    wr32(meta, 0x4E504400u);
    wr32(meta + 4, 1);
    wr32(meta + 8, 3);
    wr32(meta + 12, 1);
    size_t cid = strlen(content_id);
    memcpy(meta + 16, content_id, cid > 0x30 ? 0x30 : cid);
    for (int j = 0; j < 16; j++) {
        meta[0x40 + j] = hash[j];
        meta[0x50 + j] = (uint8_t)~hash[j];
        meta[0x60 + j] = (uint8_t)((j == 15 ? 1 : 0) ^ meta[0x50 + j] ^ 0xAA);
    }
    emit(ctx, b->data, digest);
    emit(ctx, meta, sizeof(meta));
    emit(ctx, b->data + digest + 0x80, b->len - digest - 0x80);
}

uint64_t pkg_write(const char* path, const char* content_id, const PkgItem* items, int count, int threads) {
    uint32_t* flags = xcalloc((size_t)count + 1, sizeof(uint32_t));
    uint64_t* reported = xcalloc((size_t)count + 1, sizeof(uint64_t));
    uint64_t* name_off = xcalloc((size_t)count + 1, sizeof(uint64_t));
    uint64_t* file_off = xcalloc((size_t)count + 1, sizeof(uint64_t));
    size_t* digest = xcalloc((size_t)count + 1, sizeof(size_t));
    uint64_t off = (uint64_t)count * 32;
    for (int i = 0; i < count; i++) {
        name_off[i] = off;
        off += align16(strlen(items[i].name));
    }
    for (int i = 0; i < count; i++) {
        const PkgItem* it = &items[i];
        if (it->dir) flags[i] = FLAG_DIR;
        else if (!strcmp(it->name, EBOOT_NAME)) flags[i] = FLAG_SELF;
        else flags[i] = FLAG_RAW;
        if (flags[i] == FLAG_SELF && it->data) digest[i] = self_digest(it->data->data, it->data->len);
        reported[i] = it->dir ? 0 : flags[i] == FLAG_SELF ? ((it->size - 0x30 + 63) & ~(uint64_t)63) + 0x30 : it->size;
        file_off[i] = off;
        if (!it->dir) off += align16(reported[i]);
    }
    uint64_t total = off;
    Buf desc = {0};
    for (int i = 0; i < count; i++) {
        uint8_t e[32];
        memset(e, 0, sizeof(e));
        wr32(e, (uint32_t)name_off[i]);
        wr32(e + 4, (uint32_t)strlen(items[i].name));
        wr64(e + 8, file_off[i]);
        wr64(e + 16, reported[i]);
        wr32(e + 24, flags[i]);
        buf_put(&desc, e, sizeof(e));
    }
    for (int i = 0; i < count; i++) {
        size_t n = strlen(items[i].name);
        buf_put(&desc, items[i].name, n);
        buf_zero(&desc, (size_t)(align16(n) - n));
    }

    uint8_t hdr[0x80];
    memset(hdr, 0, sizeof(hdr));
    wr32(hdr, PKG_MAGIC);
    wr32(hdr + 4, 1);
    wr32(hdr + 8, 0xC0);
    wr32(hdr + 12, 5);
    wr32(hdr + 16, 0x80);
    wr32(hdr + 20, (uint32_t)count);
    wr64(hdr + 24, total + 0x1A0);
    wr64(hdr + 32, 0x140);
    wr64(hdr + 40, total);
    size_t cid = strlen(content_id);
    memcpy(hdr + 48, content_id, cid > 0x30 ? 0x30 : cid);

    Sha1 qa;
    sha1_init(&qa);
    for (int i = 0; i < count; i++) {
        if (!items[i].dir) emit_item(&items[i], sha_emit, &qa);
    }
    sha1_update(&qa, hdr, 0x30);
    sha1_update(&qa, hdr + 0x60, 0x20);
    sha1_update(&qa, hdr + 0x60, 0x20);
    sha1_update(&qa, hdr + 0x60, 0x10);
    sha1_update(&qa, desc.data, desc.len);
    uint8_t digest_qa[20];
    sha1_final(&qa, digest_qa);
    memcpy(hdr + 0x60, digest_qa, 16);

    uint8_t ctx[64];
    key_context(hdr + 0x60, ctx);
    uint8_t lic[16];
    memset(lic, 0, sizeof(lic));
    crypt_blocks(ctx, 0xFFFFFFFFFFFFFFFFull, lic, sizeof(lic));
    memcpy(hdr + 0x70, lic, sizeof(lic));

    uint8_t meta[0x40];
    memset(meta, 0, sizeof(meta));
    static const uint32_t meta_words[12] = {1, 4, 3, 2, 4, 5, 3, 4, 0x0E, 4, 8, 0};
    for (int i = 0; i < 11; i++) wr32(meta + i * 4, meta_words[i]);
    wr32(meta + 48, (uint32_t)total);
    wr32(meta + 52, 5);
    wr32(meta + 56, 4);
    meta[60] = 0x10;
    meta[61] = 0x61;

    uint8_t hdr_sha[20], meta_sha[20], enc1[0x30], enc2[0x30], kctx[64];
    sha1(hdr, sizeof(hdr), hdr_sha);
    sha1(meta, sizeof(meta), meta_sha);
    memset(enc1, 0, sizeof(enc1));
    key_context(meta_sha + 3, kctx);
    crypt_blocks(kctx, 0, enc1, sizeof(enc1));
    memcpy(enc2, enc1, sizeof(enc2));
    key_context(hdr_sha + 3, kctx);
    crypt_blocks(kctx, 0, enc2, sizeof(enc2));

    Writer w;
    memset(&w, 0, sizeof(w));
    w.f = file_open(path, "wb");
    w.ok = w.f != NULL;
    if (w.ok) {
        fwrite(hdr, 1, sizeof(hdr), w.f);
        fwrite(hdr_sha + 3, 1, 16, w.f);
        fwrite(enc2, 1, sizeof(enc2), w.f);
        fwrite(meta, 1, sizeof(meta), w.f);
        fwrite(meta_sha + 3, 1, 16, w.f);
        fwrite(enc1, 1, sizeof(enc1), w.f);
        memcpy(w.ctx, ctx, sizeof(ctx));
        w.chunk = xmalloc(CHUNK);
        w.threads = threads;
        w.expected = total;
        writer_emit(&w, desc.data, desc.len);
        for (int i = 0; i < count && w.ok; i++) {
            const PkgItem* it = &items[i];
            if (it->dir) continue;
            uint64_t start = w.total;
            if (digest[i]) emit_self(it->data, digest[i], content_id, writer_emit, &w);
            else emit_item(it, writer_emit, &w);
            if (w.total - start != it->size) w.ok = 0;
            emit_zeros(writer_emit, &w, align16(reported[i]) - it->size);
        }
        writer_flush(&w);
        uint8_t trail[0x60];
        memset(trail, 0, sizeof(trail));
        if (fwrite(trail, 1, sizeof(trail), w.f) != sizeof(trail)) w.ok = 0;
        if (fclose(w.f) != 0) w.ok = 0;
        free(w.chunk);
        if (w.total != total) w.ok = 0;
    }
    buf_free(&desc);
    free(flags);
    free(reported);
    free(name_off);
    free(file_off);
    free(digest);
    return w.ok ? total + 0x1A0 : 0;
}

void pkg_files_free(PkgFile* files, int count) {
    for (int i = 0; i < count; i++) {
        free(files[i].name);
        buf_free(&files[i].data);
    }
    free(files);
}

int pkg_read(const uint8_t* d, size_t len, PkgFile** out, int* count, int threads) {
    if (len < 0x140 || rd32(d) != PKG_MAGIC || rd32(d + 4) != 1) return 0;
    uint32_t n = rd32(d + 20);
    uint64_t data_off = rd64(d + 32), data_size = rd64(d + 40);
    if (data_off > len || data_size > len - data_off || (uint64_t)n * 32 > data_size) return 0;
    const uint8_t* data = d + data_off;
    uint8_t ctx[64];
    key_context(d + 0x60, ctx);
    uint8_t* desc = xmalloc((size_t)n * 32 + 1);
    memcpy(desc, data, (size_t)n * 32);
    crypt_blocks(ctx, 0, desc, (size_t)n * 32);
    PkgFile* files = xcalloc((size_t)n + 1, sizeof(PkgFile));
    int ok = 1;
    for (uint32_t i = 0; i < n && ok; i++) {
        const uint8_t* e = desc + (size_t)i * 32;
        uint64_t noff = rd32(e), nlen = rd32(e + 4), foff = rd64(e + 8), fsize = rd64(e + 16);
        uint32_t flags = rd32(e + 24);
        int dir = (flags & 0xFF) == (FLAG_DIR & 0xFF);
        if (noff % 16 || noff + nlen > data_size || (!dir && (foff % 16 || foff + fsize > data_size))) {
            ok = 0;
            break;
        }
        files[i].name = xmalloc((size_t)nlen + 1);
        memcpy(files[i].name, data + noff, (size_t)nlen);
        crypt_blocks(ctx, noff / 16, (uint8_t*)files[i].name, (size_t)nlen);
        files[i].name[nlen] = 0;
        files[i].dir = dir;
        if (!dir) {
            buf_put(&files[i].data, data + foff, (size_t)fsize);
            crypt_parallel(ctx, foff / 16, files[i].data.data, files[i].data.len, threads);
        }
    }
    free(desc);
    if (!ok) {
        pkg_files_free(files, (int)n);
        return 0;
    }
    *out = files;
    *count = (int)n;
    return 1;
}
