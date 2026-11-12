#include "disc.h"

#include <stdlib.h>
#include <string.h>

#define SUB_EOR 0x01
#define SUB_DATA 0x08
#define SUB_EOF 0x80
#define SUB_LAST (SUB_EOF | SUB_DATA | SUB_EOR)
#define PVD_LBA 16
#define TERM_LBA 17
#define LPT_LBA 18
#define MPT_LBA 19
#define ROOT_LBA 20
#define POSTGAP 150
#define XA_LEN 14

static const uint8_t STAMP[7] = {126, 1, 1, 0, 0, 0, 0};
static const char LONG_STAMP[] = "2026010100000000";

static uint32_t edc_lut[256];
static uint8_t ecc_f[256];
static uint8_t ecc_b[256];
static int tables_ready;

static void init_tables(void) {
    if (tables_ready) return;
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t e = i;
        for (int k = 0; k < 8; k++) e = (e >> 1) ^ ((e & 1) ? 0xD8018001u : 0);
        edc_lut[i] = e;
        uint8_t f = (uint8_t)((i << 1) ^ ((i & 0x80) ? 0x11D : 0));
        ecc_f[i] = f;
        ecc_b[i ^ f] = (uint8_t)i;
    }
    tables_ready = 1;
}

static void le16(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void be16(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static void le32(uint8_t* p, uint32_t v) {
    le16(p, v);
    le16(p + 2, v >> 16);
}

static void be32(uint8_t* p, uint32_t v) {
    be16(p, v >> 16);
    be16(p + 2, v);
}

static void both16(uint8_t* p, uint32_t v) {
    le16(p, v);
    be16(p + 2, v);
}

static void both32(uint8_t* p, uint32_t v) {
    le32(p, v);
    be32(p + 4, v);
}

static uint32_t rd32(const uint8_t* p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

static uint8_t bcd(uint32_t v) { return (uint8_t)((v / 10) << 4 | v % 10); }

static uint32_t sectors_of(size_t len) { return len ? (uint32_t)((len + DISC_DATA - 1) / DISC_DATA) : 1; }

static void pad_str(uint8_t* p, size_t n, const char* s) {
    size_t l = strlen(s);
    memset(p, ' ', n);
    memcpy(p, s, l < n ? l : n);
}

uint32_t disc_edc(const uint8_t* p, size_t n) {
    init_tables();
    uint32_t e = 0;
    while (n--) e = (e >> 8) ^ edc_lut[(e ^ *p++) & 0xFF];
    return e;
}

static void ecc_block(const uint8_t* src, uint32_t major_n, uint32_t minor_n, uint32_t major_mult, uint32_t minor_inc, uint8_t* dst) {
    uint32_t size = major_n * minor_n;
    for (uint32_t major = 0; major < major_n; major++) {
        uint32_t idx = (major >> 1) * major_mult + (major & 1);
        uint8_t a = 0, b = 0;
        for (uint32_t minor = 0; minor < minor_n; minor++) {
            uint8_t t = src[idx];
            idx += minor_inc;
            if (idx >= size) idx -= size;
            a = ecc_f[a ^ t];
            b ^= t;
        }
        a = ecc_b[ecc_f[a] ^ b];
        dst[major] = a;
        dst[major + major_n] = a ^ b;
    }
}

void disc_seal(uint8_t* s, uint32_t lba, uint8_t submode) {
    init_tables();
    s[0] = 0;
    memset(s + 1, 0xFF, 10);
    s[11] = 0;
    memset(s + 12, 0, 12);
    s[18] = s[22] = submode;
    le32(s + 2072, disc_edc(s + 16, 8 + DISC_DATA));
    ecc_block(s + 12, 86, 24, 2, 86, s + 2076);
    ecc_block(s + 12, 52, 43, 86, 88, s + 2248);
    uint32_t a = lba + 150;
    s[12] = bcd(a / 4500);
    s[13] = bcd(a / 75 % 60);
    s[14] = bcd(a % 75);
    s[15] = 2;
}

static size_t dir_record(uint8_t* p, uint32_t lba, uint32_t size, int dir, const char* name, size_t name_len, int xa) {
    size_t len = 33 + name_len + (~name_len & 1) + (xa ? XA_LEN : 0);
    memset(p, 0, len);
    p[0] = (uint8_t)len;
    both32(p + 2, lba);
    both32(p + 10, size);
    memcpy(p + 18, STAMP, sizeof(STAMP));
    p[25] = dir ? 2 : 0;
    both16(p + 28, 1);
    p[32] = (uint8_t)name_len;
    memcpy(p + 33, name, name_len);
    if (xa) {
        uint8_t* x = p + len - XA_LEN;
        be16(x + 4, dir ? 0x8D55 : 0x0D55);
        x[6] = 'X';
        x[7] = 'A';
    }
    return len;
}

static void path_table(uint8_t* p, int big) {
    p[0] = 1;
    if (big) be32(p + 2, ROOT_LBA);
    else le32(p + 2, ROOT_LBA);
    if (big) be16(p + 6, 1);
    else le16(p + 6, 1);
}

static const DiscFile* g_sort;

static int cmp_index(const void* a, const void* b) { return strcmp(g_sort[*(const int*)a].name, g_sort[*(const int*)b].name); }

void disc_build(const char* volume, const DiscFile* files, int count, Buf* out) {
    uint32_t* lba = xmalloc(sizeof(uint32_t) * (size_t)(count + 1));
    int* order = xmalloc(sizeof(int) * (size_t)(count + 1));
    uint32_t next = ROOT_LBA + 1;
    for (int i = 0; i < count; i++) {
        uint32_t n = sectors_of(files[i].len);
        lba[i] = next;
        next += files[i].reserve > n ? files[i].reserve : n;
        order[i] = i;
    }
    uint32_t total = next + POSTGAP;
    size_t base = out->len;
    buf_zero(out, (size_t)total * DISC_RAW);
    uint8_t* bin = out->data + base;
#define USER(n) (bin + (size_t)(n) * DISC_RAW + 24)
    uint8_t* pvd = USER(PVD_LBA);
    pvd[0] = 1;
    memcpy(pvd + 1, "CD001", 5);
    pvd[6] = 1;
    pad_str(pvd + 8, 32, "PLAYSTATION");
    pad_str(pvd + 40, 32, volume);
    both32(pvd + 80, total);
    both16(pvd + 120, 1);
    both16(pvd + 124, 1);
    both16(pvd + 128, DISC_DATA);
    both32(pvd + 132, 10);
    le32(pvd + 140, LPT_LBA);
    be32(pvd + 148, MPT_LBA);
    dir_record(pvd + 156, ROOT_LBA, DISC_DATA, 1, "\0", 1, 0);
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
    memcpy(pvd + 1024, "CD-XA001", 8);
    uint8_t* term = USER(TERM_LBA);
    term[0] = 255;
    memcpy(term + 1, "CD001", 5);
    term[6] = 1;
    path_table(USER(LPT_LBA), 0);
    path_table(USER(MPT_LBA), 1);
    g_sort = files;
    qsort(order, (size_t)count, sizeof(int), cmp_index);
    uint8_t* dir = USER(ROOT_LBA);
    size_t pos = dir_record(dir, ROOT_LBA, DISC_DATA, 1, "\0", 1, 1);
    pos += dir_record(dir + pos, ROOT_LBA, DISC_DATA, 1, "\1", 1, 1);
    for (int k = 0; k < count; k++) {
        const DiscFile* f = &files[order[k]];
        char name[40];
        size_t n = (size_t)snprintf(name, sizeof(name), "%s;1", f->name);
        if (pos + 33 + n + 1 + XA_LEN > DISC_DATA) fatal("disc: too many files for one directory sector");
        pos += dir_record(dir + pos, lba[order[k]], (uint32_t)f->len, 0, name, n, 1);
    }
    for (int i = 0; i < count; i++) {
        for (size_t off = 0; off < files[i].len; off += DISC_DATA) {
            size_t n = files[i].len - off;
            memcpy(USER(lba[i] + off / DISC_DATA), files[i].data + off, n < DISC_DATA ? n : DISC_DATA);
        }
    }
    uint8_t* sub = xcalloc(total, 1);
    sub[PVD_LBA] = SUB_DATA | SUB_EOR;
    memset(sub + TERM_LBA, SUB_LAST, ROOT_LBA - TERM_LBA + 1);
    for (int i = 0; i < count; i++) {
        uint32_t used = sectors_of(files[i].len);
        memset(sub + lba[i], SUB_DATA, used - 1);
        sub[lba[i] + used - 1] = SUB_LAST;
    }
    for (uint32_t s = 0; s < total; s++) disc_seal(bin + (size_t)s * DISC_RAW, s, sub[s]);
    free(sub);
#undef USER
    free(order);
    free(lba);
}

typedef struct {
    size_t record;
    uint32_t dir_lba;
    uint32_t lba;
    uint32_t size;
    uint32_t room;
} Located;

static int locate(const uint8_t* bin, size_t len, const char* name, Located* out) {
    size_t sectors = len / DISC_RAW;
    if (sectors <= ROOT_LBA) return 0;
    const uint8_t* pvd = bin + (size_t)PVD_LBA * DISC_RAW + 24;
    if (pvd[0] != 1 || memcmp(pvd + 1, "CD001", 5) != 0) return 0;
    uint32_t root = rd32(pvd + 158), dir_sectors = sectors_of(rd32(pvd + 166));
    if (root + dir_sectors > sectors) return 0;
    size_t want = strlen(name);
    uint32_t next = UINT32_MAX;
    out->record = 0;
    for (int pass = 0; pass < 2; pass++) {
        for (uint32_t s = 0; s < dir_sectors; s++) {
            size_t at = (size_t)(root + s) * DISC_RAW + 24;
            for (size_t pos = 0; pos < DISC_DATA && bin[at + pos]; pos += bin[at + pos]) {
                const uint8_t* r = bin + at + pos;
                if (r[25] & 2) continue;
                uint32_t lba = rd32(r + 2);
                if (pass) {
                    if (lba > out->lba && lba < next) next = lba;
                    continue;
                }
                const char* n = (const char*)r + 33;
                const char* semi = memchr(n, ';', r[32]);
                size_t nl = semi ? (size_t)(semi - n) : r[32];
                if (nl != want || memcmp(n, name, want)) continue;
                out->record = at + pos;
                out->dir_lba = root + s;
                out->lba = lba;
                out->size = rd32(r + 10);
            }
        }
        if (!out->record) return 0;
    }
    out->room = next == UINT32_MAX ? sectors_of(out->size) : next - out->lba;
    return 1;
}

int disc_find(const uint8_t* bin, size_t len, const char* name, uint32_t* lba, uint32_t* size, uint32_t* room) {
    Located at;
    if (!locate(bin, len, name, &at)) return 0;
    *lba = at.lba;
    *size = at.size;
    *room = at.room;
    return 1;
}

int disc_replace(uint8_t* bin, size_t len, const char* name, const uint8_t* data, size_t size) {
    Located at;
    if (!locate(bin, len, name, &at)) return 0;
    uint32_t need = sectors_of(size);
    if (need > at.room || (size_t)(at.lba + at.room) * DISC_RAW > len) return 0;
    for (uint32_t i = 0; i < at.room; i++) {
        uint8_t* s = bin + (size_t)(at.lba + i) * DISC_RAW;
        size_t off = (size_t)i * DISC_DATA;
        memset(s + 24, 0, DISC_DATA);
        if (off < size) memcpy(s + 24, data + off, size - off < DISC_DATA ? size - off : DISC_DATA);
        disc_seal(s, at.lba + i, i + 1 == need ? SUB_LAST : i < need ? SUB_DATA : 0);
    }
    both32(bin + at.record + 10, (uint32_t)size);
    uint8_t* dir = bin + (size_t)at.dir_lba * DISC_RAW;
    disc_seal(dir, at.dir_lba, dir[18]);
    return 1;
}

void disc_cue(const char* bin_name, Buf* out) {
    char* s = str_fmt("FILE \"%s\" BINARY\n  TRACK 01 MODE2/2352\n    INDEX 01 00:00:00\n", bin_name);
    buf_put(out, s, strlen(s));
    free(s);
}

int exe_check(const uint8_t* d, size_t len) {
    if (len < DISC_DATA || len % DISC_DATA || len > (size_t)DISC_EXE_SECTORS * DISC_DATA) return 0;
    return !memcmp(d, "PS-X EXE", 8) && rd32(d + 0x1C) == len - DISC_DATA;
}

int ps1_disc_build(const uint8_t* exe, size_t exe_len, const uint8_t* pak, size_t pak_len, Buf* out) {
    static const char CNF[] = "BOOT = cdrom:\\" DISC_EXE ";1\r\nTCB = 4\r\nEVENT = 10\r\nSTACK = 801FFFF0\r\n";
    if (!exe_check(exe, exe_len)) return 0;
    DiscFile files[3] = {
        {"SYSTEM.CNF", (const uint8_t*)CNF, sizeof(CNF) - 1, 0},
        {DISC_EXE, exe, exe_len, DISC_EXE_SECTORS},
        {DISC_PAK, pak, pak_len, 0},
    };
    disc_build("TCOAAL", files, 3, out);
    return 1;
}
