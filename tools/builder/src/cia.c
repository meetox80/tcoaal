#include "cia.h"

#include <string.h>

#include "crypto.h"
#include "pack.h"

#define MEDIA_UNIT 0x200
#define ROMFS_ALIGN 0x1000
#define CBMD_HEADER 0x88
#define IVFC_BLOCK 0x1000
#define SMDH_SIZE 0x36C0
#define TMD_HEADER 0x140
#define TMD_INFO (TMD_HEADER + 0xC4)
#define TMD_CHUNK (TMD_INFO + 0x900)

static uint32_t le32(const uint8_t* p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t le64(const uint8_t* p) { return (uint64_t)le32(p) | (uint64_t)le32(p + 4) << 32; }
static uint16_t be16(const uint8_t* p) { return (uint16_t)(p[0] << 8 | p[1]); }

static void put_le32(uint8_t* p, uint32_t v) {
    for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i));
}

static void put_le64(uint8_t* p, uint64_t v) {
    for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i));
}

static void put_be64(uint8_t* p, uint64_t v) {
    for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (56 - 8 * i));
}

static size_t align_up(size_t v, size_t a) { return (v + a - 1) / a * a; }

static void pad_to(Buf* b, size_t start, size_t a) { buf_zero(b, align_up(b->len - start, a) - (b->len - start)); }

static void hash_blocks(const uint8_t* data, size_t len, Buf* out) {
    uint8_t block[IVFC_BLOCK], h[32];
    for (size_t off = 0; off < len; off += IVFC_BLOCK) {
        size_t n = len - off < IVFC_BLOCK ? len - off : IVFC_BLOCK;
        if (n == IVFC_BLOCK) {
            sha256(data + off, IVFC_BLOCK, h);
        } else {
            memset(block, 0, sizeof(block));
            memcpy(block, data + off, n);
            sha256(block, IVFC_BLOCK, h);
        }
        buf_put(out, h, sizeof(h));
    }
}

static void put_level(Buf* out, uint64_t offset, uint64_t size) {
    buf_u32(out, (uint32_t)offset);
    buf_u32(out, (uint32_t)(offset >> 32));
    buf_u32(out, (uint32_t)size);
    buf_u32(out, (uint32_t)(size >> 32));
    buf_u32(out, 12);
    buf_u32(out, 0);
}

void ivfc_build(const uint8_t* level3, size_t len, Buf* out) {
    Buf l2 = {0}, l1 = {0}, master = {0};
    hash_blocks(level3, len, &l2);
    hash_blocks(l2.data, l2.len, &l1);
    hash_blocks(l1.data, l1.len, &master);
    uint64_t off2 = align_up(l1.len, IVFC_BLOCK);
    uint64_t off3 = align_up(off2 + l2.len, IVFC_BLOCK);
    size_t start = out->len;
    buf_put(out, "IVFC", 4);
    buf_u32(out, 0x10000);
    buf_u32(out, (uint32_t)master.len);
    put_level(out, 0, l1.len);
    put_level(out, off2, l2.len);
    put_level(out, off3, len);
    buf_u32(out, 0x5C);
    buf_u32(out, 0);
    buf_u32(out, 0);
    buf_put(out, master.data, master.len);
    pad_to(out, start, IVFC_BLOCK);
    buf_put(out, level3, len);
    pad_to(out, start, IVFC_BLOCK);
    buf_put(out, l1.data, l1.len);
    pad_to(out, start, IVFC_BLOCK);
    buf_put(out, l2.data, l2.len);
    pad_to(out, start, IVFC_BLOCK);
    buf_free(&l2);
    buf_free(&l1);
    buf_free(&master);
}

static int lz11_decode(const uint8_t* in, size_t len, Buf* out) {
    if (len < 4 || in[0] != 0x11) return 0;
    size_t want = (size_t)in[1] | (size_t)in[2] << 8 | (size_t)in[3] << 16, pos = 4;
    size_t start = out->len;
    while (out->len - start < want) {
        if (pos >= len) return 0;
        uint8_t flags = in[pos++];
        for (int bit = 7; bit >= 0 && out->len - start < want; bit--) {
            if (!(flags >> bit & 1)) {
                if (pos >= len) return 0;
                buf_u8(out, in[pos++]);
                continue;
            }
            if (pos + 2 > len) return 0;
            uint8_t b0 = in[pos], b1 = in[pos + 1];
            size_t n, disp;
            if (b0 >> 4 == 0) {
                if (pos + 3 > len) return 0;
                n = ((size_t)(b0 & 0xF) << 4 | b1 >> 4) + 0x11;
                disp = ((size_t)(b1 & 0xF) << 8 | in[pos + 2]) + 1;
                pos += 3;
            } else if (b0 >> 4 == 1) {
                if (pos + 4 > len) return 0;
                n = ((size_t)(b0 & 0xF) << 12 | (size_t)b1 << 4 | in[pos + 2] >> 4) + 0x111;
                disp = ((size_t)(in[pos + 2] & 0xF) << 8 | in[pos + 3]) + 1;
                pos += 4;
            } else {
                n = (size_t)(b0 >> 4) + 1;
                disp = ((size_t)(b0 & 0xF) << 8 | b1) + 1;
                pos += 2;
            }
            if (disp > out->len - start || out->len - start + n > want) return 0;
            for (size_t k = 0; k < n; k++) buf_u8(out, out->data[out->len - disp]);
        }
    }
    return 1;
}

static void lz11_store(const uint8_t* in, size_t len, Buf* out) {
    uint8_t head[4] = {0x11, (uint8_t)len, (uint8_t)(len >> 8), (uint8_t)(len >> 16)};
    buf_put(out, head, 4);
    for (size_t off = 0; off < len; off += 8) {
        buf_u8(out, 0);
        buf_put(out, in + off, len - off < 8 ? len - off : 8);
    }
}

static int banner_build(const uint8_t* tpl, size_t len, const CiaLook* look, Buf* out) {
    if (len < CBMD_HEADER || memcmp(tpl, "CBMD", 4) != 0) return 0;
    uint32_t cgfx = le32(tpl + 8), cwav = le32(tpl + 0x84);
    if (cgfx != CBMD_HEADER || cwav <= cgfx || cwav > len) return 0;
    for (int i = 1; i < 14; i++) {
        if (le32(tpl + 8 + i * 4)) return 0;
    }
    size_t start = out->len;
    buf_put(out, tpl, CBMD_HEADER);
    if (look->banner) {
        Buf gfx = {0};
        int ok = lz11_decode(tpl + cgfx, cwav - cgfx, &gfx) && gfx.len > CTR_BANNER_SIZE && !memcmp(gfx.data, "CGFX", 4);
        if (ok) {
            memcpy(gfx.data + gfx.len - CTR_BANNER_SIZE, look->banner, CTR_BANNER_SIZE);
            lz11_store(gfx.data, gfx.len, out);
        }
        buf_free(&gfx);
        if (!ok) {
            out->len = start;
            return 0;
        }
    } else {
        buf_put(out, tpl + cgfx, cwav - cgfx);
    }
    pad_to(out, start, 16);
    put_le32(out->data + start + 0x84, (uint32_t)(out->len - start));
    if (look->sound) buf_put(out, look->sound, look->sound_len);
    else buf_put(out, tpl + cwav, len - cwav);
    return 1;
}

static void exefs_build(const uint8_t* src, const CiaLook* look, Buf* out) {
    size_t start = out->len;
    buf_zero(out, MEDIA_UNIT);
    for (int i = 0; i < 10; i++) {
        const uint8_t* e = src + i * 16;
        const uint8_t* data = src + MEDIA_UNIT + le32(e + 8);
        size_t size = le32(e + 12);
        if (!size) continue;
        Buf bnr = {0};
        if (look->smdh && !memcmp(e, "icon", 5) && size == SMDH_SIZE) {
            data = look->smdh;
        } else if ((look->banner || look->sound) && !memcmp(e, "banner", 7) && banner_build(data, size, look, &bnr)) {
            data = bnr.data;
            size = bnr.len;
        }
        uint8_t h[32];
        sha256(data, size, h);
        size_t off = out->len - start - MEDIA_UNIT;
        buf_put(out, data, size);
        pad_to(out, start, MEDIA_UNIT);
        buf_free(&bnr);
        uint8_t* head = out->data + start;
        memcpy(head + i * 16, e, 8);
        put_le32(head + i * 16 + 8, (uint32_t)off);
        put_le32(head + i * 16 + 12, (uint32_t)size);
        memcpy(head + 0xC0 + (9 - i) * 0x20, h, 32);
    }
}

int cia_build(const uint8_t* tpl, size_t tpl_len, const CiaLook* look, const uint8_t* level3, size_t level3_len, Buf* out) {
    if (tpl_len < 0x2020) return 0;
    uint32_t header = le32(tpl), certs = le32(tpl + 8), ticket = le32(tpl + 12), tmd = le32(tpl + 16), meta = le32(tpl + 20);
    uint64_t content = le64(tpl + 24);
    size_t ticket_off = align_up(align_up(header, 64) + certs, 64);
    size_t tmd_off = align_up(ticket_off + ticket, 64);
    size_t content_off = align_up(tmd_off + tmd, 64);
    size_t meta_off = align_up(content_off + content, 64);
    if (meta_off + meta > tpl_len || tmd < TMD_CHUNK + 0x30 || be16(tpl + tmd_off + TMD_HEADER + 0x9E) != 1) return 0;
    const uint8_t* ncch = tpl + content_off;
    if (content < 0x200 || memcmp(ncch + 0x100, "NCCH", 4) != 0) return 0;
    size_t exefs_off = (size_t)le32(ncch + 0x1A0) * MEDIA_UNIT;
    size_t exefs_len = (size_t)le32(ncch + 0x1A4) * MEDIA_UNIT;
    size_t romfs_off = (size_t)le32(ncch + 0x1B0) * MEDIA_UNIT;
    if (!romfs_off || romfs_off > content || exefs_len < MEDIA_UNIT || exefs_off + exefs_len > romfs_off) return 0;

    size_t start = out->len;
    buf_put(out, tpl, content_off);
    size_t n0 = out->len;
    buf_put(out, ncch, exefs_off);
    exefs_build(ncch + exefs_off, look, out);
    exefs_len = out->len - n0 - exefs_off;
    pad_to(out, n0, ROMFS_ALIGN);
    size_t r0 = out->len;
    ivfc_build(level3, level3_len, out);
    size_t rlen = out->len - r0;
    uint32_t region = (uint32_t)(align_up(0x60 + le32(out->data + r0 + 8), MEDIA_UNIT) / MEDIA_UNIT);
    uint8_t* nh = out->data + n0;
    put_le32(nh + 0x1A4, (uint32_t)(exefs_len / MEDIA_UNIT));
    sha256(nh + exefs_off, (size_t)le32(nh + 0x1A8) * MEDIA_UNIT, nh + 0x1C0);
    put_le32(nh + 0x1B0, (uint32_t)((r0 - n0) / MEDIA_UNIT));
    put_le32(nh + 0x1B4, (uint32_t)(rlen / MEDIA_UNIT));
    put_le32(nh + 0x1B8, region);
    sha256(out->data + r0, (size_t)region * MEDIA_UNIT, nh + 0x1E0);
    size_t nlen = out->len - n0;
    put_le32(nh + 0x104, (uint32_t)(nlen / MEDIA_UNIT));

    uint8_t* t = out->data + start + tmd_off;
    uint8_t* chunk = t + TMD_CHUNK;
    put_be64(chunk + 8, nlen);
    sha256(out->data + n0, nlen, chunk + 0x10);
    sha256(chunk, 0x30, t + TMD_INFO + 4);
    sha256(t + TMD_INFO, 0x900, t + TMD_HEADER + 0xA4);
    put_le64(out->data + start + 24, nlen);

    pad_to(out, start, 64);
    size_t m0 = out->len;
    buf_put(out, tpl + meta_off, meta);
    if (look->smdh && meta >= 0x400 + SMDH_SIZE) memcpy(out->data + m0 + 0x400, look->smdh, SMDH_SIZE);
    return 1;
}
