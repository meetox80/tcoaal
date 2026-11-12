#include "iso.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void le16(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void be16(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static void le32(uint8_t* p, uint32_t v) {
    le16(p, v & 0xFFFF);
    le16(p + 2, v >> 16);
}

static void be32(uint8_t* p, uint32_t v) {
    be16(p, v >> 16);
    be16(p + 2, v & 0xFFFF);
}

static void both16(uint8_t* p, uint32_t v) {
    le16(p, v);
    be16(p + 2, v);
}

static void both32(uint8_t* p, uint32_t v) {
    le32(p, v);
    be32(p + 4, v);
}

static uint32_t rd_le32(const uint8_t* p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

static void pad_str(uint8_t* p, size_t n, const char* s) {
    memset(p, ' ', n);
    size_t l = strlen(s);
    memcpy(p, s, l < n ? l : n);
}

static const uint8_t STAMP[7] = {126, 1, 1, 0, 0, 0, 0};
static const char* LONG_STAMP = "2026010100000000";

static size_t dir_record(uint8_t* p, uint32_t lba, uint32_t size, int dir, const uint8_t* name, size_t name_len) {
    size_t len = 33 + name_len + ((name_len & 1) ? 0 : 1);
    memset(p, 0, len);
    p[0] = (uint8_t)len;
    both32(p + 2, lba);
    both32(p + 10, size);
    memcpy(p + 18, STAMP, 7);
    p[25] = dir ? 2 : 0;
    both16(p + 28, 1);
    p[32] = (uint8_t)name_len;
    memcpy(p + 33, name, name_len);
    return len;
}

static int cmp_file(const void* a, const void* b) { return strcmp(((const IsoFile*)a)->name, ((const IsoFile*)b)->name); }

static uint32_t sectors(size_t len) { return (uint32_t)((len + ISO_SECTOR - 1) / ISO_SECTOR); }

void iso_build(const char* volume, const IsoFile* files, int count, Buf* out) {
    IsoFile* sorted = xmalloc(sizeof(IsoFile) * (size_t)(count ? count : 1));
    memcpy(sorted, files, sizeof(IsoFile) * (size_t)count);
    qsort(sorted, (size_t)count, sizeof(IsoFile), cmp_file);
    const uint32_t root = 20;
    uint32_t* lba = xmalloc(sizeof(uint32_t) * (size_t)(count ? count : 1));
    uint32_t next = root + 1;
    for (int i = 0; i < count; i++) {
        lba[i] = next;
        next += sectors(sorted[i].len);
    }
    uint32_t total = next;
    size_t base = out->len;
    buf_reserve(out, (size_t)total * ISO_SECTOR);
    buf_zero(out, (size_t)(root + 1) * ISO_SECTOR);
    uint8_t* iso = out->data + base;
    uint8_t* pvd = iso + 16 * ISO_SECTOR;
    pvd[0] = 1;
    memcpy(pvd + 1, "CD001", 5);
    pvd[6] = 1;
    pad_str(pvd + 8, 32, "PLAYSTATION");
    pad_str(pvd + 40, 32, volume);
    both32(pvd + 80, total);
    both16(pvd + 120, 1);
    both16(pvd + 124, 1);
    both16(pvd + 128, ISO_SECTOR);
    both32(pvd + 132, 10);
    le32(pvd + 140, 18);
    be32(pvd + 148, 19);
    dir_record(pvd + 156, root, ISO_SECTOR, 1, (const uint8_t*)"\0", 1);
    pad_str(pvd + 190, 128, "");
    pad_str(pvd + 318, 128, "");
    pad_str(pvd + 446, 128, "");
    pad_str(pvd + 574, 128, "PLAYSTATION");
    pad_str(pvd + 702, 37, "");
    pad_str(pvd + 739, 37, "");
    pad_str(pvd + 776, 37, "");
    memcpy(pvd + 813, LONG_STAMP, 16);
    memcpy(pvd + 830, LONG_STAMP, 16);
    memset(pvd + 847, '0', 16);
    memset(pvd + 864, '0', 16);
    pvd[881] = 1;
    uint8_t* term = iso + 17 * ISO_SECTOR;
    term[0] = 255;
    memcpy(term + 1, "CD001", 5);
    term[6] = 1;
    uint8_t* lpt = iso + 18 * ISO_SECTOR;
    lpt[0] = 1;
    le32(lpt + 2, root);
    le16(lpt + 6, 1);
    uint8_t* mpt = iso + 19 * ISO_SECTOR;
    mpt[0] = 1;
    be32(mpt + 2, root);
    be16(mpt + 6, 1);
    uint8_t* dir = iso + root * ISO_SECTOR;
    size_t pos = 0;
    pos += dir_record(dir + pos, root, ISO_SECTOR, 1, (const uint8_t*)"\0", 1);
    pos += dir_record(dir + pos, root, ISO_SECTOR, 1, (const uint8_t*)"\1", 1);
    for (int i = 0; i < count; i++) {
        char name[64];
        size_t n = (size_t)snprintf(name, sizeof(name), "%s;1", sorted[i].name);
        pos += dir_record(dir + pos, lba[i], (uint32_t)sorted[i].len, 0, (const uint8_t*)name, n);
    }
    for (int i = 0; i < count; i++) {
        buf_put(out, sorted[i].data, sorted[i].len);
        buf_zero(out, (size_t)sectors(sorted[i].len) * ISO_SECTOR - sorted[i].len);
    }
    free(lba);
    free(sorted);
}

int iso_find(const uint8_t* iso, size_t len, const char* name, size_t* off, size_t* size) {
    if (len < 17 * ISO_SECTOR) return 0;
    const uint8_t* pvd = iso + 16 * ISO_SECTOR;
    if (pvd[0] != 1 || memcmp(pvd + 1, "CD001", 5) != 0) return 0;
    uint32_t root = rd_le32(pvd + 156 + 2), root_len = rd_le32(pvd + 156 + 10);
    if ((size_t)root * ISO_SECTOR + root_len > len) return 0;
    const uint8_t* dir = iso + (size_t)root * ISO_SECTOR;
    size_t want = strlen(name);
    size_t pos = 0;
    while (pos < root_len) {
        uint8_t rl = dir[pos];
        if (!rl) {
            pos = (pos / ISO_SECTOR + 1) * ISO_SECTOR;
            continue;
        }
        const uint8_t* r = dir + pos;
        size_t nl = r[32];
        const char* n = (const char*)r + 33;
        size_t base_len = nl;
        for (size_t i = 0; i < nl; i++) {
            if (n[i] == ';') {
                base_len = i;
                break;
            }
        }
        if (!(r[25] & 2) && base_len == want && !memcmp(n, name, want)) {
            size_t o = (size_t)rd_le32(r + 2) * ISO_SECTOR, s = rd_le32(r + 10);
            if (o + s > len) return 0;
            *off = o;
            *size = s;
            return 1;
        }
        pos += rl;
    }
    return 0;
}

int elf_check(const uint8_t* data, size_t len) {
    if (len < 52 || memcmp(data, "\x7f" "ELF", 4) != 0) return 0;
    return data[4] == 1 && data[5] == 1 && data[18] == 8 && data[19] == 0;
}

void ps2_system_cnf(Buf* out) {
    const char* cnf = "BOOT2 = cdrom0:\\" PS2_BOOT_NAME ";1\r\nVER = 1.00\r\nVMODE = NTSC\r\n";
    buf_put(out, cnf, strlen(cnf));
}

int ps2_iso_build(const uint8_t* elf, size_t elf_len, const uint8_t* pak, size_t pak_len, Buf* out) {
    if (!elf_check(elf, elf_len)) return 0;
    Buf cnf = {0};
    ps2_system_cnf(&cnf);
    IsoFile files[3] = {
        {"SYSTEM.CNF", cnf.data, cnf.len},
        {PS2_BOOT_NAME, elf, elf_len},
        {PS2_PAK_NAME, pak, pak_len},
    };
    iso_build("TCOAAL", files, 3, out);
    buf_free(&cnf);
    return 1;
}
