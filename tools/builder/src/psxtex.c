#include "psxtex.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#define ALPHA_CLEAR 40
#define ALPHA_SOLID 208
#define DITHER_FLOOR 4
#define CELLS 32768
#define KMEANS_PASSES 3

static const int W[3] = {3, 4, 2};

typedef struct {
    int c[3];
    uint32_t n;
} Color;

typedef struct {
    double n, s[3], q[3];
} Stat;

typedef struct {
    int lo, hi, cut;
    double gain;
} Box;

static int to5(int v) { return (v * 31 + 127) / 255; }
static int to8(int v) { return v << 3 | v >> 2; }
static int clamp8(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }
static int cell(const int* c) { return to5(c[0]) | to5(c[1]) << 5 | to5(c[2]) << 10; }

int psx_scaled(int v) {
    int s = (v + PSX_DIV / 2) / PSX_DIV;
    return s > 0 ? s : 1;
}

static void unpremultiply(Image* img) {
    size_t n = (size_t)img->w * (size_t)img->h;
    for (size_t i = 0; i < n; i++) {
        uint8_t* p = img->px + i * 4;
        if (!p[3] || p[3] == 255) continue;
        for (int k = 0; k < 3; k++) p[k] = (uint8_t)clamp8((p[k] * 255 + p[3] / 2) / p[3]);
    }
}

Image* psx_resize(const Image* straight) { return psx_resize_cells(straight, 1, 1); }

Image* psx_resize_cells(const Image* straight, int cols, int rows) {
    if (cols <= 0 || rows <= 0) cols = rows = 1;
    int w = straight->w, h = straight->h, ow = psx_scaled(w), oh = psx_scaled(h);
    Image* out = img_new(ow, oh);
    for (int r = 0; r < rows; r++) {
        for (int c = 0; c < cols; c++) {
            int x0 = w * c / cols, x1 = w * (c + 1) / cols, y0 = h * r / rows, y1 = h * (r + 1) / rows;
            int dx0 = ow * c / cols, dx1 = ow * (c + 1) / cols, dy0 = oh * r / rows, dy1 = oh * (r + 1) / rows;
            if (x1 <= x0 || y1 <= y0 || dx1 <= dx0 || dy1 <= dy0) continue;
            Image* cell = img_crop(straight, x0, y0, x1, y1);
            img_premultiply(cell);
            Image* small = img_resize(cell, dx1 - dx0, dy1 - dy0, FILTER_LANCZOS);
            unpremultiply(small);
            img_paste(out, small, dx0, dy0);
            img_free(small);
            img_free(cell);
        }
    }
    return out;
}

static void stat_add(Stat* s, const Color* c) {
    s->n += c->n;
    for (int k = 0; k < 3; k++) {
        double v = (double)c->n * c->c[k];
        s->s[k] += v;
        s->q[k] += v * c->c[k];
    }
}

static double stat_sse(const Stat* s) {
    if (s->n <= 0) return 0;
    double e = 0;
    for (int k = 0; k < 3; k++) e += W[k] * (s->q[k] - s->s[k] * s->s[k] / s->n);
    return e;
}

static Stat stat_minus(const Stat* a, const Stat* b) {
    Stat r = {a->n - b->n, {0}, {0}};
    for (int k = 0; k < 3; k++) {
        r.s[k] = a->s[k] - b->s[k];
        r.q[k] = a->q[k] - b->q[k];
    }
    return r;
}

static int cmp0(const void* a, const void* b) { return ((const Color*)a)->c[0] - ((const Color*)b)->c[0]; }
static int cmp1(const void* a, const void* b) { return ((const Color*)a)->c[1] - ((const Color*)b)->c[1]; }
static int cmp2(const void* a, const void* b) { return ((const Color*)a)->c[2] - ((const Color*)b)->c[2]; }

static int (*const CMP[3])(const void*, const void*) = {cmp0, cmp1, cmp2};

static void plan(Color* col, Box* b) {
    b->gain = 0;
    b->cut = b->lo;
    if (b->hi - b->lo < 2) return;
    Stat all = {0, {0}, {0}};
    for (int i = b->lo; i < b->hi; i++) stat_add(&all, &col[i]);
    int axis = 0;
    double spread = -1;
    for (int k = 0; k < 3; k++) {
        double v = W[k] * (all.q[k] - all.s[k] * all.s[k] / all.n);
        if (v > spread) {
            spread = v;
            axis = k;
        }
    }
    qsort(col + b->lo, (size_t)(b->hi - b->lo), sizeof(Color), CMP[axis]);
    double whole = stat_sse(&all), best = whole;
    Stat left = {0, {0}, {0}};
    for (int i = b->lo; i < b->hi - 1; i++) {
        stat_add(&left, &col[i]);
        if (col[i].c[axis] == col[i + 1].c[axis]) continue;
        Stat right = stat_minus(&all, &left);
        double e = stat_sse(&left) + stat_sse(&right);
        if (e < best) {
            best = e;
            b->cut = i + 1;
        }
    }
    b->gain = whole - best;
}

static int nearest(const int (*pal)[3], int count, const int* c) {
    int best = 0, dist = INT_MAX;
    for (int i = 0; i < count; i++) {
        int dr = pal[i][0] - c[0], dg = pal[i][1] - c[1], db = pal[i][2] - c[2];
        int d = W[0] * dr * dr + W[1] * dg * dg + W[2] * db * db;
        if (d < dist) {
            dist = d;
            best = i;
        }
    }
    return best;
}

static void centroid(const Stat* s, int* out) {
    for (int k = 0; k < 3; k++) out[k] = to8(to5(clamp8((int)(s->s[k] / s->n + 0.5))));
}

static int quantize(Color* col, int m, int k, int (*pal)[3]) {
    if (m <= k) {
        for (int i = 0; i < m; i++) {
            for (int c = 0; c < 3; c++) pal[i][c] = to8(to5(col[i].c[c]));
        }
        return m;
    }
    Box* box = xmalloc(sizeof(Box) * (size_t)k);
    box[0] = (Box){0, m, 0, 0};
    plan(col, &box[0]);
    int nb = 1;
    while (nb < k) {
        int pick = -1;
        double gain = 0;
        for (int i = 0; i < nb; i++) {
            if (box[i].gain > gain) {
                gain = box[i].gain;
                pick = i;
            }
        }
        if (pick < 0) break;
        box[nb] = (Box){box[pick].cut, box[pick].hi, 0, 0};
        box[pick].hi = box[pick].cut;
        plan(col, &box[pick]);
        plan(col, &box[nb]);
        nb++;
    }
    for (int i = 0; i < nb; i++) {
        Stat s = {0, {0}, {0}};
        for (int j = box[i].lo; j < box[i].hi; j++) stat_add(&s, &col[j]);
        centroid(&s, pal[i]);
    }
    free(box);
    Stat* acc = xmalloc(sizeof(Stat) * (size_t)nb);
    for (int pass = 0; pass < KMEANS_PASSES; pass++) {
        memset(acc, 0, sizeof(Stat) * (size_t)nb);
        for (int i = 0; i < m; i++) stat_add(&acc[nearest((const int(*)[3])pal, nb, col[i].c)], &col[i]);
        for (int i = 0; i < nb; i++) {
            if (acc[i].n > 0) centroid(&acc[i], pal[i]);
        }
    }
    free(acc);
    return nb;
}

enum { CLEAR, SOLID, SEMI };

typedef struct {
    int w, h;
    uint8_t* cls;
    uint8_t* rgb;
    int count[3];
} Work;

static Work prepare(const Image* img, int x0, int y0, int w, int h) {
    Work wk = {w, h, xmalloc((size_t)w * h + 1), xmalloc((size_t)w * h * 3 + 1), {0, 0, 0}};
    for (int y = 0; y < h; y++) {
        const uint8_t* p = img->px + ((size_t)(y0 + y) * img->w + x0) * 4;
        for (int x = 0; x < w; x++, p += 4) {
            size_t i = (size_t)y * w + x;
            int a = p[3], c = a < ALPHA_CLEAR ? CLEAR : a >= ALPHA_SOLID ? SOLID : SEMI;
            wk.cls[i] = (uint8_t)c;
            wk.count[c]++;
            for (int k = 0; k < 3; k++) wk.rgb[i * 3 + k] = (uint8_t)(c == SEMI ? clamp8((2 * a * p[k] + 127) / 255) : p[k]);
        }
    }
    return wk;
}

static void release_work(Work* wk) {
    free(wk->cls);
    free(wk->rgb);
}

static int build_palette(const Work* wk, int cls, int k, int (*pal)[3]) {
    if (k <= 0 || !wk->count[cls]) return 0;
    uint32_t(*bins)[4] = xcalloc(CELLS, sizeof(*bins));
    for (size_t i = 0; i < (size_t)wk->w * wk->h; i++) {
        if (wk->cls[i] != cls) continue;
        int c[3] = {wk->rgb[i * 3], wk->rgb[i * 3 + 1], wk->rgb[i * 3 + 2]};
        uint32_t* b = bins[cell(c)];
        for (int j = 0; j < 3; j++) b[j] += (uint32_t)c[j];
        b[3]++;
    }
    Color* col = xmalloc(sizeof(Color) * CELLS);
    int m = 0;
    for (int i = 0; i < CELLS; i++) {
        uint32_t n = bins[i][3];
        if (!n) continue;
        for (int c = 0; c < 3; c++) col[m].c[c] = (int)((bins[i][c] + n / 2) / n);
        col[m++].n = n;
    }
    free(bins);
    int count = quantize(col, m, k, pal);
    free(col);
    return count;
}

typedef struct {
    const int (*pal)[3];
    int count;
    int base;
    int16_t* cache;
} Subset;

static uint16_t pick(Subset* s, const int* c, int* q) {
    int key = cell(c);
    if (!s->pal) {
        for (int k = 0; k < 3; k++) q[k] = to8(to5(c[k]));
        return (uint16_t)key;
    }
    if (s->cache[key] < 0) {
        int rep[3] = {to8(key & 31), to8(key >> 5 & 31), to8(key >> 10)};
        s->cache[key] = (int16_t)nearest(s->pal, s->count, rep);
    }
    memcpy(q, s->pal[s->cache[key]], sizeof(int) * 3);
    return (uint16_t)(s->base + s->cache[key]);
}

static void dither(const Work* wk, Subset* sets, uint16_t* out) {
    int w = wk->w;
    size_t row = (size_t)(w + 2) * 3;
    int32_t* errs = xcalloc(row * 2, sizeof(int32_t));
    int32_t* cur = errs;
    int32_t* nxt = errs + row;
    for (int y = 0; y < wk->h; y++) {
        int dir = (y & 1) ? -1 : 1;
        memset(nxt, 0, row * sizeof(int32_t));
        for (int i = 0; i < w; i++) {
            int x = dir > 0 ? i : w - 1 - i;
            size_t at = (size_t)y * w + x;
            int cls = wk->cls[at];
            if (cls == CLEAR) {
                out[at] = 0;
                continue;
            }
            int32_t* e = cur + (size_t)(x + 1) * 3;
            int c[3], q[3];
            for (int k = 0; k < 3; k++) c[k] = clamp8(wk->rgb[at * 3 + k] + e[k] / 16);
            out[at] = pick(&sets[cls], c, q);
            int ahead = x + dir >= 0 && x + dir < w && wk->cls[at + dir] == cls;
            int below = y + 1 < wk->h;
            for (int k = 0; k < 3; k++) {
                int32_t err = c[k] - q[k];
                if (err >= -DITHER_FLOOR && err <= DITHER_FLOOR) continue;
                if (ahead) e[dir * 3 + k] += err * 7;
                if (!below) continue;
                size_t b = at + (size_t)w;
                if (x - dir >= 0 && x - dir < w && wk->cls[b - dir] == cls) nxt[(size_t)(x + 1 - dir) * 3 + k] += err * 3;
                if (wk->cls[b] == cls) nxt[(size_t)(x + 1) * 3 + k] += err * 5;
                if (x + dir >= 0 && x + dir < w && wk->cls[b + dir] == cls) nxt[(size_t)(x + 1 + dir) * 3 + k] += err;
            }
        }
        int32_t* t = cur;
        cur = nxt;
        nxt = t;
    }
    free(errs);
}

static uint16_t clut_entry(const int* c, int stp) {
    uint16_t v = (uint16_t)cell(c);
    if (stp) return (uint16_t)(0x8000 | v);
    return v ? v : 0x0421;
}

int psx_tex_encode(const Image* img, int orig_w, int orig_h, PsxDepth depth, Buf* out) {
    int x0 = img->w, y0 = img->h, x1 = 0, y1 = 0;
    for (int y = 0; y < img->h; y++) {
        const uint8_t* p = img->px + (size_t)y * img->w * 4;
        for (int x = 0; x < img->w; x++) {
            if (p[x * 4 + 3] < ALPHA_CLEAR) continue;
            if (x < x0) x0 = x;
            if (x >= x1) x1 = x + 1;
            if (y < y0) y0 = y;
            y1 = y + 1;
        }
    }
    int w = x1 > x0 ? x1 - x0 : 0, h = y1 > y0 ? y1 - y0 : 0;
    if (!w || !h) x0 = y0 = 0;
    Work wk = prepare(img, x0, y0, w, h);
    int holes = wk.count[CLEAR] > 0, semi = wk.count[SEMI] > 0;
    int base = holes ? 1 : 0;
    int entries = depth == PSX_4BIT ? 16 : depth == PSX_8BIT ? 256 : 0;
    int budget = entries - base, visible = wk.count[SOLID] + wk.count[SEMI];
    int ks = 0;
    if (semi && budget > 0) {
        ks = wk.count[SOLID] ? (budget * wk.count[SEMI] + visible / 2) / visible : budget;
        if (ks < 1) ks = 1;
        if (wk.count[SOLID] && ks > budget - 1) ks = budget - 1;
    }
    int solid_pal[256][3], semi_pal[256][3];
    int ns = entries ? build_palette(&wk, SOLID, budget - ks, solid_pal) : 0;
    int nm = entries ? build_palette(&wk, SEMI, ks, semi_pal) : 0;
    Subset sets[3] = {{NULL, 0, 0, NULL}, {NULL, ns, base, NULL}, {NULL, nm, base + ns, NULL}};
    if (entries) {
        sets[SOLID].pal = (const int(*)[3])solid_pal;
        sets[SEMI].pal = (const int(*)[3])semi_pal;
        for (int c = SOLID; c <= SEMI; c++) {
            sets[c].cache = xmalloc(sizeof(int16_t) * CELLS);
            memset(sets[c].cache, 0xFF, sizeof(int16_t) * CELLS);
        }
    }
    uint16_t* tex = xmalloc(sizeof(uint16_t) * ((size_t)w * h + 1));
    dither(&wk, sets, tex);
    if (!entries) {
        for (size_t i = 0; i < (size_t)w * h; i++) {
            if (wk.cls[i] == CLEAR) continue;
            int c[3] = {to8(tex[i] & 31), to8(tex[i] >> 5 & 31), to8(tex[i] >> 10)};
            tex[i] = clut_entry(c, wk.cls[i] == SEMI || !semi);
        }
    }
    int bits = depth == PSX_4BIT ? 4 : depth == PSX_8BIT ? 8 : 16;
    int per = 16 / bits;
    int nx = (w + PSX_CHUNK - 1) / PSX_CHUNK, ny = (h + PSX_CHUNK - 1) / PSX_CHUNK;
    int cw = nx ? ((w + nx - 1) / nx + per - 1) / per * per : 0;
    int ch = ny ? (h + ny - 1) / ny : 0;
    buf_put(out, "PTEX", 4);
    buf_u8(out, (uint8_t)depth);
    buf_u8(out, (uint8_t)(holes | semi << 1));
    buf_u16(out, (uint16_t)(nx * ny));
    buf_u16(out, (uint16_t)w);
    buf_u16(out, (uint16_t)h);
    buf_u16(out, (uint16_t)x0);
    buf_u16(out, (uint16_t)y0);
    buf_u16(out, (uint16_t)img->w);
    buf_u16(out, (uint16_t)img->h);
    buf_u16(out, (uint16_t)orig_w);
    buf_u16(out, (uint16_t)orig_h);
    for (int i = 0; i < entries; i++) {
        int j = i - base;
        if (j >= 0 && j < ns) buf_u16(out, clut_entry(solid_pal[j], !semi));
        else if (j >= ns && j < ns + nm) buf_u16(out, clut_entry(semi_pal[j - ns], 1));
        else buf_u16(out, 0);
    }
    for (int cy = 0; cy < h; cy += ch) {
        for (int cx = 0; cx < w; cx += cw) {
            buf_u16(out, (uint16_t)cx);
            buf_u16(out, (uint16_t)cy);
            buf_u16(out, (uint16_t)(cx + cw < w ? cw : w - cx));
            buf_u16(out, (uint16_t)(cy + ch < h ? ch : h - cy));
        }
    }
    for (int cy = 0; cy < h; cy += ch) {
        int rows = cy + ch < h ? ch : h - cy;
        for (int cx = 0; cx < w; cx += cw) {
            int cols = cx + cw < w ? cw : w - cx;
            int words = (cols + per - 1) / per;
            for (int r = 0; r < rows; r++) {
                const uint16_t* src = tex + (size_t)(cy + r) * w + cx;
                for (int i = 0; i < words; i++) {
                    uint16_t v = 0;
                    for (int t = 0; t < per && i * per + t < cols; t++) v |= (uint16_t)(src[i * per + t] << (t * bits));
                    buf_u16(out, v);
                }
            }
            if ((words * rows) & 1) buf_u16(out, 0);
        }
    }
    free(sets[SOLID].cache);
    free(sets[SEMI].cache);
    free(tex);
    release_work(&wk);
    return 1;
}

int psx_card_icon(const uint8_t* png, size_t len, Buf* out) {
    Image* img = img_load_png(png, len);
    if (!img) return 0;
    Image* pm = img_clone(img);
    img_premultiply(pm);
    Image* small = img_resize(pm, 16, 16, FILTER_LANCZOS);
    img_free(pm);
    img_free(img);
    unpremultiply(small);
    for (int i = 0; i < 256; i++) {
        uint8_t* p = small->px + i * 4;
        if (p[3] >= ALPHA_CLEAR) p[3] = 255;
    }
    Work wk = prepare(small, 0, 0, 16, 16);
    int base = wk.count[CLEAR] > 0;
    int pal[16][3];
    int n = build_palette(&wk, SOLID, 16 - base, pal);
    Subset sets[3] = {{NULL, 0, 0, NULL}, {(const int(*)[3])pal, n, base, NULL}, {NULL, 0, 0, NULL}};
    sets[SOLID].cache = xmalloc(sizeof(int16_t) * CELLS);
    memset(sets[SOLID].cache, 0xFF, sizeof(int16_t) * CELLS);
    uint16_t tex[256];
    dither(&wk, sets, tex);
    for (int i = 0; i < 16; i++) {
        int j = i - base;
        buf_u16(out, j >= 0 && j < n ? clut_entry(pal[j], 0) : 0);
    }
    for (int i = 0; i < 128; i++) buf_u8(out, (uint8_t)(tex[i * 2] | tex[i * 2 + 1] << 4));
    free(sets[SOLID].cache);
    release_work(&wk);
    img_free(small);
    return 1;
}

int psx_tex_from_png(const uint8_t* png, size_t len, int cols, int rows, PsxDepth depth, Buf* out) {
    Image* img = img_load_png(png, len);
    if (!img) return 0;
    Image* small = psx_resize_cells(img, cols, rows);
    int ok = psx_tex_encode(small, img->w, img->h, depth, out);
    img_free(small);
    img_free(img);
    return ok;
}
