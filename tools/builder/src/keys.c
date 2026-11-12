#include "keys.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "target.h"

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#include "../third_party/stb_truetype.h"
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

typedef struct {
    uint8_t r, g, b, a;
} Rgba;

static const Rgba BLUE = {183, 220, 240, 255};
static const Rgba GREEN = {157, 215, 98, 255};
static const Rgba GREEN_LABEL = {211, 248, 170, 255};
static const Rgba RED = {246, 161, 139, 255};
static const Rgba YELLOW = {215, 202, 27, 255};
static const Rgba YELLOW_LABEL = {244, 247, 170, 255};
static const Rgba PURPLE = {207, 162, 233, 255};
static const Rgba PURPLE_LINE = {150, 90, 210, 255};

#define SS 4
#define BUTTON_R 60.0

typedef struct {
    double x, y;
} Pt;

static void put_px(Image* img, int x, int y, Rgba c) {
    if (x < 0 || y < 0 || x >= img->w || y >= img->h) return;
    uint8_t* p = img->px + ((size_t)y * img->w + x) * 4;
    p[0] = c.r;
    p[1] = c.g;
    p[2] = c.b;
    p[3] = c.a;
}

static void fill_polygon(Image* img, const Pt* pts, int n, Rgba c) {
    double ymin = pts[0].y, ymax = pts[0].y;
    for (int i = 1; i < n; i++) {
        if (pts[i].y < ymin) ymin = pts[i].y;
        if (pts[i].y > ymax) ymax = pts[i].y;
    }
    int j0 = (int)floor(ymin), j1 = (int)ceil(ymax);
    double xs[64];
    for (int j = j0; j <= j1; j++) {
        double sy = j + 0.5;
        int nx = 0;
        for (int i = 0; i < n; i++) {
            Pt a = pts[i], b = pts[(i + 1) % n];
            if ((a.y <= sy && b.y > sy) || (b.y <= sy && a.y > sy)) {
                if (nx < 64) xs[nx++] = a.x + (sy - a.y) * (b.x - a.x) / (b.y - a.y);
            }
        }
        for (int i = 1; i < nx; i++) {
            double v = xs[i];
            int k = i - 1;
            while (k >= 0 && xs[k] > v) {
                xs[k + 1] = xs[k];
                k--;
            }
            xs[k + 1] = v;
        }
        for (int i = 0; i + 1 < nx; i += 2) {
            int x0 = (int)ceil(xs[i] - 0.5), x1 = (int)ceil(xs[i + 1] - 0.5);
            for (int x = x0; x < x1; x++) put_px(img, x, j, c);
        }
    }
}

static void fill_ellipse(Image* img, double x0, double y0, double x1, double y1, Rgba c) {
    double cx = (x0 + x1) / 2, cy = (y0 + y1) / 2, rx = (x1 - x0) / 2, ry = (y1 - y0) / 2;
    if (rx <= 0 || ry <= 0) return;
    for (int j = (int)floor(y0); j <= (int)ceil(y1); j++) {
        for (int i = (int)floor(x0); i <= (int)ceil(x1); i++) {
            double dx = (i + 0.5 - cx) / rx, dy = (j + 0.5 - cy) / ry;
            if (dx * dx + dy * dy <= 1.0) put_px(img, i, j, c);
        }
    }
}

static void wide_segment(Image* img, double ax, double ay, double bx, double by, double width, Rgba c) {
    double dx = bx - ax, dy = by - ay;
    double n = sqrt(dx * dx + dy * dy);
    if (n < 1e-9) return;
    double px = -dy / n * width / 2, py = dx / n * width / 2;
    Pt q[4] = {{ax + px + 0.5, ay + py + 0.5}, {bx + px + 0.5, by + py + 0.5}, {bx - px + 0.5, by - py + 0.5}, {ax - px + 0.5, ay - py + 0.5}};
    fill_polygon(img, q, 4, c);
}

static void symbol(Image* img, const char* kind, int cx, int cy, int r, Rgba c, int width) {
    if (!strcmp(kind, "cross")) {
        wide_segment(img, cx - r, cy - r, cx + r, cy + r, width, c);
        wide_segment(img, cx - r, cy + r, cx + r, cy - r, width, c);
    } else if (!strcmp(kind, "circle")) {
        double ro = r + 0.5, ri = ro - width;
        for (int j = cy - r; j <= cy + r; j++) {
            for (int i = cx - r; i <= cx + r; i++) {
                double d = sqrt((double)(i - cx) * (i - cx) + (double)(j - cy) * (j - cy));
                if (d <= ro && d > ri) put_px(img, i, j, c);
            }
        }
    } else if (!strcmp(kind, "triangle")) {
        double pts[4][2] = {{cx, cy - r}, {cx + r, cy + r * 0.8}, {cx - r, cy + r * 0.8}, {cx, cy - r}};
        for (int i = 0; i < 3; i++) wide_segment(img, pts[i][0], pts[i][1], pts[i + 1][0], pts[i + 1][1], width, c);
        for (int i = 0; i < 3; i++) fill_ellipse(img, pts[i][0] - width / 2.0 + 0.5, pts[i][1] - width / 2.0 + 0.5, pts[i][0] + width / 2.0 + 0.5, pts[i][1] + width / 2.0 + 0.5, c);
    } else if (!strcmp(kind, "square")) {
        int x0 = cx - r, y0 = cy - r, x1 = cx + r, y1 = cy + r;
        for (int j = y0; j <= y1; j++) {
            for (int i = x0; i <= x1; i++) {
                if (i < x0 + width || i > x1 - width || j < y0 + width || j > y1 - width) put_px(img, i, j, c);
            }
        }
    }
}

static int cmp_int(const void* a, const void* b) { return *(const int*)a - *(const int*)b; }

static Image* blank_cap(const Image* src, int bx0, int by0, int bx1, int by1) {
    const int keep = 8;
    Image* cap = img_crop(src, bx0, by0, bx1, by1);
    int w = cap->w, h = cap->h;
    int n = 0;
    int* lums = xmalloc(sizeof(int) * (size_t)(w * h + 1));
    for (int y = keep; y < h - keep; y++) {
        for (int x = keep; x < w - keep; x++) {
            const uint8_t* p = cap->px + ((size_t)y * w + x) * 4;
            if (p[3] > 200) lums[n++] = p[0] + p[1] + p[2];
        }
    }
    uint8_t fill[3] = {60, 60, 60};
    if (n > 0) {
        int* sorted = xmalloc(sizeof(int) * (size_t)n);
        memcpy(sorted, lums, sizeof(int) * (size_t)n);
        qsort(sorted, (size_t)n, sizeof(int), cmp_int);
        double pos = (n - 1) * 0.5;
        int lo = (int)floor(pos);
        int hi = lo + 1 < n ? lo + 1 : lo;
        double t = pos - lo;
        double pct = sorted[lo] + (sorted[hi] - sorted[lo]) * t;
        free(sorted);
        double sum[3] = {0, 0, 0};
        int cnt = 0;
        for (int y = keep; y < h - keep; y++) {
            for (int x = keep; x < w - keep; x++) {
                const uint8_t* p = cap->px + ((size_t)y * w + x) * 4;
                if (p[3] > 200 && p[0] + p[1] + p[2] <= pct) {
                    sum[0] += p[0];
                    sum[1] += p[1];
                    sum[2] += p[2];
                    cnt++;
                }
            }
        }
        for (int c = 0; c < 3; c++) fill[c] = (uint8_t)(sum[c] / cnt);
    }
    free(lums);
    for (int y = keep; y < h - keep; y++) {
        for (int x = keep; x < w - keep; x++) {
            uint8_t* p = cap->px + ((size_t)y * w + x) * 4;
            p[0] = fill[0];
            p[1] = fill[1];
            p[2] = fill[2];
            p[3] = 255;
        }
    }
    return cap;
}

static Image* square_cap(const Image* src, int x0, int y0, int x1, int y1) {
    int h = y1 - y0;
    int half = h / 2;
    Image* left = img_crop(src, x0, y0, x0 + half, y1);
    Image* right = img_crop(src, x1 - (h - half), y0, x1, y1);
    Image* out = img_new(h, h);
    img_paste(out, left, 0, 0);
    img_paste(out, right, half, 0);
    img_free(left);
    img_free(right);
    return out;
}

static void composite_owned(Image* dst, Image* src, int x, int y) {
    img_alpha_composite(dst, src, x, y);
    img_free(src);
}

typedef struct {
    stbtt_fontinfo info;
    float scale;
    int ascent;
} Face;

static int face_init(Face* f, const uint8_t* ttf, int px) {
    int off = stbtt_GetFontOffsetForIndex(ttf, 0);
    if (off < 0 || !stbtt_InitFont(&f->info, ttf, off)) return 0;
    f->scale = stbtt_ScaleForMappingEmToPixels(&f->info, (float)px);
    int asc, desc, gap;
    stbtt_GetFontVMetrics(&f->info, &asc, &desc, &gap);
    f->ascent = (int)ceil(asc * f->scale);
    return 1;
}

static uint8_t blend8(uint8_t a, uint8_t b, uint8_t m) {
    int tmp = ((int)b - (int)a) * m + 128;
    return (uint8_t)((int)a + (((tmp >> 8) + tmp) >> 8));
}

static void draw_text(Image* img, const Face* f, double x, double baseline, const char* text, Rgba c) {
    double pen = x;
    for (const char* s = text; *s; s++) {
        int cp = (unsigned char)*s;
        int adv, lsb;
        stbtt_GetCodepointHMetrics(&f->info, cp, &adv, &lsb);
        double fx = floor(pen), fy = floor(baseline);
        float sx = (float)(pen - fx), sy = (float)(baseline - fy);
        int x0, y0, x1, y1;
        stbtt_GetCodepointBitmapBoxSubpixel(&f->info, cp, f->scale, f->scale, sx, sy, &x0, &y0, &x1, &y1);
        int w = x1 - x0, h = y1 - y0;
        if (w > 0 && h > 0) {
            uint8_t* bm = xcalloc((size_t)w * (size_t)h, 1);
            stbtt_MakeCodepointBitmapSubpixel(&f->info, bm, w, h, w, f->scale, f->scale, sx, sy, cp);
            for (int j = 0; j < h; j++) {
                for (int i = 0; i < w; i++) {
                    uint8_t m = bm[j * w + i];
                    if (!m) continue;
                    int px = (int)fx + x0 + i, py = (int)fy + y0 + j;
                    if (px < 0 || py < 0 || px >= img->w || py >= img->h) continue;
                    uint8_t* p = img->px + ((size_t)py * img->w + px) * 4;
                    p[0] = blend8(p[0], c.r, m);
                    p[1] = blend8(p[1], c.g, m);
                    p[2] = blend8(p[2], c.b, m);
                    p[3] = blend8(p[3], c.a, m);
                }
            }
            free(bm);
        }
        pen += adv * f->scale;
        if (s[1]) pen += stbtt_GetCodepointKernAdvance(&f->info, cp, (unsigned char)s[1]) * f->scale;
    }
}

static void draw_text_la(Image* img, const Face* f, double x, double y, const char* text, Rgba c) {
    draw_text(img, f, x, y + f->ascent, text, c);
}

static void draw_text_centered(Image* img, const Face* f, double cx, double cy, const char* text, Rgba c) {
    int x0, y0, x1, y1;
    stbtt_GetCodepointBitmapBox(&f->info, (unsigned char)text[0], f->scale, f->scale, &x0, &y0, &x1, &y1);
    double left = x0, right = x1, pen = 0;
    for (const char* s = text; *s; s++) {
        int cp = (unsigned char)*s, adv, lsb, gx0, gy0, gx1, gy1;
        stbtt_GetCodepointBitmapBox(&f->info, cp, f->scale, f->scale, &gx0, &gy0, &gx1, &gy1);
        if (s == text) left = pen + gx0;
        right = pen + gx1;
        stbtt_GetCodepointHMetrics(&f->info, cp, &adv, &lsb);
        pen += adv * f->scale;
        if (s[1]) pen += stbtt_GetCodepointKernAdvance(&f->info, cp, (unsigned char)s[1]) * f->scale;
    }
    draw_text(img, f, cx - (left + right) / 2.0, cy - (y0 + y1) / 2.0, text, c);
}

typedef enum { ACT_MOVE, ACT_RUN, ACT_INTERACT, ACT_CANCEL, ACT_MENU, ACT_FF, ACT_COUNT } Action;

typedef struct {
    Action act;
    int edge;
    double x, y;
    int rel_x, rel_y;
    double ex, ey;
    double lx, ly, anchor_x, anchor_y;
} Line;

typedef struct {
    double view_x, view_y, view_w;
    int pos_x, pos_y;
    double label_scale;
    int letters;
    const char* ff_cap;
    int ff_big;
    const char* caps[4];
    Line lines[ACT_COUNT];
} Layout;

static const Layout PSP_LAYOUT = {
    -16.0, -16.0, 2032.0, 780, 300, 1.0, 0, "R", 1, {0},
    {
        {ACT_MOVE, 0, 100, 400, 1, 0, -8, 512, -26, 2, 0, 0},
        {ACT_RUN, 1, 1657.0, 388.2, 0, 0, 1084, 512, -18, 2, 0, 0},
        {ACT_INTERACT, 1, 1771.1, 502.6, 0, 0, 1162, 512, -36, 2, 0, 0},
        {ACT_CANCEL, 1, 1885.3, 387.3, 0, 1, 1198, 0, 4, -12, 0, 0},
        {ACT_MENU, 1, 1772.4, 273.3, 0, 0, 1192, 302, -2, -23, 0, 0},
        {ACT_FF, 0, 1730, 28, 0, 0, 1062, 248, -57, -46, 0, 0},
    },
};

static const Layout CTR_LAYOUT = {
    0.0, 0.0, 1600.0, 750, 245, 1.4, 1, "R", 1, {"A", "B", "X", "Y"},
    {
        {ACT_MOVE, 0, 260, 700, 1, 0, 0, 492, 0, 2, 0.5, 0},
        {ACT_RUN, 1, 1222, 420, 0, 0, 1025, 492, 0, 2, 0.5, 0},
        {ACT_CANCEL, 1, 1340, 538, 0, 0, 1100, 492, 0, 2, 0.25, 0},
        {ACT_INTERACT, 1, 1458, 420, 0, 1, 1156, 0, 4, -15, 0, 0},
        {ACT_MENU, 1, 1340, 302, 0, 0, 1150, 290, 2, -31, 0, 0},
        {ACT_FF, 0, 1340, 44, 0, 0, 1040, 205, 0, -63, 0.5, 0},
    },
};

#define DUALSHOCK_LINES                                              \
    {                                                                \
        {ACT_MOVE, 0, 355, 440, 1, 0, 0, 470, 0, 3, 0.5, 0},         \
        {ACT_RUN, 1, 1150, 318, 0, 0, 1035, 470, 0, 3, 0.5, 0},      \
        {ACT_INTERACT, 1, 1245, 413, 0, 0, 1125, 470, 0, 3, 0.5, 0}, \
        {ACT_CANCEL, 1, 1340, 318, 0, 0, 1228, 470, 0, 3, 0.5, 0},   \
        {ACT_MENU, 1, 1245, 223, 0, 0, 990, 205, 0, -3, 0.5, 1},     \
        {ACT_FF, 0, 1245, 70, 1, 0, 0, 176, 0, -3, 0.5, 1},          \
    }

static const Layout PS2_LAYOUT = {0.0, 0.0, 1600.0, 780, 190, 1.3, 0, "R1", 0, {0}, DUALSHOCK_LINES};
static const Layout PS3_LAYOUT = {0.0, 0.0, 1600.0, 780, 190, 1.0, 0, "R1", 0, {0}, DUALSHOCK_LINES};
static const Layout PS1_LAYOUT = {0.0, 0.0, 1600.0, 780, 190, 1.3, 0, "R1", 0, {0}, DUALSHOCK_LINES};

static const Layout VITA_LAYOUT = {
    0.0, 0.0, 1600.0, 775, 250, 1.0, 0, "R", 1, {0},
    {
        {ACT_MOVE, 0, 270, 470, 1, 0, 0, 500, 0, 2, 0.5, 0},
        {ACT_RUN, 1, 1244, 380, 0, 0, 1075, 500, 0, 2, 0.5, 0},
        {ACT_INTERACT, 1, 1328, 464, 0, 0, 1165, 500, 0, 2, 0.5, 0},
        {ACT_CANCEL, 1, 1412, 380, 0, 1, 1200, 0, 4, -12, 0, 0},
        {ACT_MENU, 1, 1328, 296, 0, 0, 1060, 262, 0, -3, 0.5, 1},
        {ACT_FF, 0, 1444, 153, 0, 0, 1175, 215, 0, -3, 0.5, 1},
    },
};

static const Layout WII_LAYOUT = {
    0.0, 0.0, 1600.0, 775, 200, 1.2, 1, "A", 1, {"2", "1", "+", "B"},
    {
        {ACT_MOVE, 0, 300, 520, 0, 0, 840, 440, 0, 3, 0.5, 0},
        {ACT_RUN, 0, 570, 186, 1, 0, 0, 205, 0, -3, 0.5, 1},
        {ACT_INTERACT, 1, 1260, 450, 0, 0, 1195, 440, 0, 3, 0.5, 0},
        {ACT_CANCEL, 1, 1120, 450, 0, 0, 1090, 440, 0, 3, 0.5, 0},
        {ACT_MENU, 0, 806, 336, 0, 0, 1045, 225, 0, -3, 0.5, 1},
        {ACT_FF, 1, 570, 450, 0, 0, 960, 440, 0, 3, 0.5, 0},
    },
};

static const Layout WIIU_LAYOUT = {
    0.0, 0.0, 1600.0, 770, 200, 1.0, 1, "R", 1, {"A", "B", "X", "Y"},
    {
        {ACT_MOVE, 0, 230, 610, 1, 0, 0, 470, 0, 3, 0.5, 0},
        {ACT_RUN, 1, 1280, 540, 0, 0, 1075, 470, 0, 3, 0.5, 0},
        {ACT_INTERACT, 1, 1460, 540, 0, 0, 1240, 470, 0, 3, 0.5, 0},
        {ACT_CANCEL, 1, 1370, 630, 0, 0, 1150, 470, 0, 3, 0.5, 0},
        {ACT_MENU, 1, 1370, 450, 0, 0, 1245, 293, 0, -3, 0.5, 1},
        {ACT_FF, 0, 1340, 135, 1, 0, 0, 176, 0, -3, 0.5, 1},
    },
};

static const Layout XENON_LAYOUT = {
    0.0, 0.0, 1600.0, 780, 190, 1.0, 1, "RB", 0, {"A", "B", "Y", "X"},
    {
        {ACT_MOVE, 0, 585, 590, 1, 0, 0, 470, 0, 3, 0.5, 0},
        {ACT_RUN, 1, 1105, 320, 0, 0, 1035, 470, 0, 3, 0.5, 0},
        {ACT_INTERACT, 1, 1200, 415, 0, 0, 1130, 470, 0, 3, 0.5, 0},
        {ACT_CANCEL, 1, 1295, 320, 0, 0, 1232, 470, 0, 3, 0.5, 0},
        {ACT_MENU, 1, 1200, 225, 0, 0, 990, 205, 0, -3, 0.5, 1},
        {ACT_FF, 0, 1190, 112, 1, 0, 0, 176, 0, -3, 0.5, 1},
    },
};

static const Layout* layout_for(TargetId id) {
    switch (id) {
        case TARGET_VITA:
            return &VITA_LAYOUT;
        case TARGET_3DS:
            return &CTR_LAYOUT;
        case TARGET_WII:
            return &WII_LAYOUT;
        case TARGET_WIIU:
            return &WIIU_LAYOUT;
        case TARGET_XENON:
            return &XENON_LAYOUT;
        case TARGET_PS2:
            return &PS2_LAYOUT;
        case TARGET_PS3:
            return &PS3_LAYOUT;
        case TARGET_PS1:
            return &PS1_LAYOUT;
        default:
            return &PSP_LAYOUT;
    }
}

static Pt at(const Layout* lay, double scale, double x, double y) {
    Pt p = {lay->pos_x + (x - lay->view_x) * scale, lay->pos_y + (y - lay->view_y) * scale};
    return p;
}

static void tapered(Image* lines, Pt a, Pt b, Rgba c) {
    const double w0 = 5.0, w1 = 2.0;
    double dx = b.x - a.x, dy = b.y - a.y;
    double n = sqrt(dx * dx + dy * dy);
    if (n < 1e-6) n = 1e-6;
    double px = -dy / n, py = dx / n;
    Pt q[4] = {
        {(a.x + px * w0 / 2) * SS, (a.y + py * w0 / 2) * SS},
        {(b.x + px * w1 / 2) * SS, (b.y + py * w1 / 2) * SS},
        {(b.x - px * w1 / 2) * SS, (b.y - py * w1 / 2) * SS},
        {(a.x - px * w0 / 2) * SS, (a.y - py * w0 / 2) * SS},
    };
    fill_polygon(lines, q, 4, c);
    fill_ellipse(lines, (a.x - w0 / 2) * SS, (a.y - w0 / 2) * SS, (a.x + w0 / 2) * SS, (a.y + w0 / 2) * SS, c);
}

static Pt edge_pt(const Layout* lay, double scale, double cx0, double cy0, Pt toward) {
    Pt c = at(lay, scale, cx0, cy0);
    double dx = toward.x - c.x, dy = toward.y - c.y;
    double n = sqrt(dx * dx + dy * dy);
    if (n < 1e-6) n = 1e-6;
    double rr = BUTTON_R * scale;
    Pt p = {c.x + dx / n * rr, c.y + dy / n * rr};
    return p;
}

static void unpremultiply(Image* img) {
    size_t n = (size_t)img->w * (size_t)img->h;
    for (size_t i = 0; i < n; i++) {
        uint8_t* p = img->px + i * 4;
        unsigned a = p[3];
        if (a == 0 || a == 255) continue;
        for (int c = 0; c < 3; c++) {
            unsigned v = 255u * p[c] / a;
            p[c] = (uint8_t)(v > 255 ? 255 : v);
        }
    }
}

static void label(Image* out, const Image* src, const int* box, double scale, double x, double y, double anchor_x, double anchor_y, const int* erase) {
    Image* crop = img_crop(src, box[0], box[1], box[2], box[3]);
    if (erase) img_fill_rect(crop, erase[0], erase[1], erase[2], erase[3], 0);
    if (scale != 1.0) {
        img_premultiply(crop);
        Image* big = img_resize(crop, (int)rint(crop->w * scale), (int)rint(crop->h * scale), FILTER_LANCZOS);
        img_free(crop);
        unpremultiply(big);
        crop = big;
    }
    img_alpha_composite(out, crop, (int)rint(x - crop->w * anchor_x), (int)rint(y - crop->h * anchor_y));
    img_free(crop);
}

static const Rgba* line_color(Action act) {
    static const Rgba* const COLORS[ACT_COUNT] = {&BLUE, &YELLOW, &GREEN, &RED, &RED, &PURPLE_LINE};
    return COLORS[act];
}

static const int LABEL_BOX[ACT_COUNT][4] = {
    {867, 510, 919, 531},
    {981, 494, 1018, 515},
    {1056, 445, 1129, 466},
    {1111, 349, 1176, 371},
    {1099, 300, 1151, 321},
    {1035, 219, 1150, 264},
};

static void face_glyph(Image* out, const Face* f, int letters, const char* letter, const char* kind, int cx, int cy, int sym_cy, int r, Rgba c) {
    if (letters) {
        draw_text_centered(out, f, cx, cy, letter, c);
    } else {
        symbol(out, kind, cx, sym_cy, r, c, 4);
    }
}

int keys_swappable(void) {
    switch (g_target->id) {
        case TARGET_PSP:
        case TARGET_VITA:
        case TARGET_PS2:
        case TARGET_PS3: return 1;
        default: return 0;
    }
}

static const Rgba BUTTON_GREEN_FILL = {0x46, 0x60, 0x2c, 255};
static const Rgba BUTTON_GREEN_RING = {0x9d, 0xd7, 0x62, 255};
static const Rgba BUTTON_RED_FILL = {0x6e, 0x48, 0x3e, 255};
static const Rgba BUTTON_RED_RING = {0xf6, 0xa1, 0x8b, 255};

static int cmp_u8(const void* a, const void* b) { return *(const uint8_t*)a - *(const uint8_t*)b; }

static void recolor_button(Image* art, double cx, double cy, double r, Rgba fill_from, Rgba ring_from, Rgba fill_to, Rgba ring_to) {
    uint8_t ring[3][720];
    int n = 0;
    for (int i = 0; i < 720 && n < 720; i++) {
        double a = i * 3.14159265358979 / 360.0;
        int x = (int)lrint(cx + cos(a) * (r + 5)), y = (int)lrint(cy + sin(a) * (r + 5));
        if (x < 0 || y < 0 || x >= art->w || y >= art->h) continue;
        const uint8_t* p = art->px + ((size_t)y * art->w + x) * 4;
        if (p[3] < 250) continue;
        for (int c = 0; c < 3; c++) ring[c][n] = p[c];
        n++;
    }
    if (n == 0) return;
    double bg[3];
    for (int c = 0; c < 3; c++) {
        qsort(ring[c], (size_t)n, 1, cmp_u8);
        bg[c] = ring[c][n / 2];
    }
    double f[3] = {fill_from.r - bg[0], fill_from.g - bg[1], fill_from.b - bg[2]};
    double s[3] = {ring_from.r - bg[0], ring_from.g - bg[1], ring_from.b - bg[2]};
    double ff = 0, ss = 0, fs = 0;
    for (int c = 0; c < 3; c++) {
        ff += f[c] * f[c];
        ss += s[c] * s[c];
        fs += f[c] * s[c];
    }
    double det = ff * ss - fs * fs;
    if (fabs(det) < 1e-6) return;
    const double to_fill[3] = {fill_to.r, fill_to.g, fill_to.b}, to_ring[3] = {ring_to.r, ring_to.g, ring_to.b};
    int x0 = (int)floor(cx - r), x1 = (int)ceil(cx + r), y0 = (int)floor(cy - r), y1 = (int)ceil(cy + r);
    for (int y = y0; y <= y1; y++) {
        for (int x = x0; x <= x1; x++) {
            if (x < 0 || y < 0 || x >= art->w || y >= art->h) continue;
            double dx = x + 0.5 - cx, dy = y + 0.5 - cy;
            if (dx * dx + dy * dy > r * r) continue;
            uint8_t* p = art->px + ((size_t)y * art->w + x) * 4;
            if (!p[3]) continue;
            double d[3] = {p[0] - bg[0], p[1] - bg[1], p[2] - bg[2]};
            double df = 0, ds = 0;
            for (int c = 0; c < 3; c++) {
                df += d[c] * f[c];
                ds += d[c] * s[c];
            }
            double wf = (df * ss - ds * fs) / det, ws = (ds * ff - df * fs) / det;
            if (wf < 0) wf = 0;
            if (ws < 0) ws = 0;
            if (wf + ws > 1) {
                double t = wf + ws;
                wf /= t;
                ws /= t;
            }
            if (wf + ws < 0.02) continue;
            for (int c = 0; c < 3; c++) {
                double v = bg[c] + wf * (to_fill[c] - bg[c]) + ws * (to_ring[c] - bg[c]);
                p[c] = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : lrint(v));
            }
        }
    }
}

static void swap_buttons(Image* art, const Layout* lay, double scale) {
    for (int i = 0; i < ACT_COUNT; i++) {
        const Line* ln = &lay->lines[i];
        if (ln->act != ACT_INTERACT && ln->act != ACT_CANCEL) continue;
        double cx = (ln->x - lay->view_x) * scale, cy = (ln->y - lay->view_y) * scale, r = (BUTTON_R + 4) * scale;
        if (ln->act == ACT_INTERACT) recolor_button(art, cx, cy, r, BUTTON_GREEN_FILL, BUTTON_GREEN_RING, BUTTON_RED_FILL, BUTTON_RED_RING);
        else recolor_button(art, cx, cy, r, BUTTON_RED_FILL, BUTTON_RED_RING, BUTTON_GREEN_FILL, BUTTON_GREEN_RING);
    }
}

static Action swapped(Action act, int swap) {
    if (!swap) return act;
    if (act == ACT_INTERACT) return ACT_CANCEL;
    if (act == ACT_CANCEL) return ACT_INTERACT;
    return act;
}

Image* keys_build(const uint8_t* keys_png, size_t keys_len, const uint8_t* ttf, size_t ttf_len, const uint8_t* art_png, size_t art_len, int swap) {
    (void)ttf_len;
    const Layout* lay = layout_for(g_target->id);
    Image* src = img_load_png(keys_png, keys_len);
    Image* art = img_load_png(art_png, art_len);
    Face big, small;
    if (!src || !art || !face_init(&big, ttf, 30) || !face_init(&small, ttf, 22)) {
        img_free(src);
        img_free(art);
        return NULL;
    }
    Image* out = img_new(src->w, src->h);
    Image* t = img_crop(src, 0, 0, 740, 545);
    img_paste(out, t, 0, 0);
    img_free(t);
    img_fill_rect(out, 330, 100, 740, 530, 0);
    const int ys[4] = {197, 274, 375, 446};
    for (int i = 0; i < 4; i++) {
        t = img_crop(src, 330, ys[i], 740, ys[i] + 9);
        img_paste(out, t, 330, ys[i]);
        img_free(t);
    }
    t = img_crop(src, 330, 535, 740, 545);
    img_paste(out, t, 330, 535);
    img_free(t);

    composite_owned(out, img_crop(src, 540, 104, 668, 192), 365, 104);

    composite_owned(out, blank_cap(src, 365, 219, 409, 263), 365, 219);
    face_glyph(out, &big, lay->letters, lay->caps[0], swap ? "circle" : "cross", 365 + 22, 219 + 22, 219 + 22, swap ? 11 : 10, GREEN_LABEL);

    Image* red_cap = blank_cap(src, 420, 305, 464, 349);
    img_alpha_composite(out, red_cap, 365, 305);
    face_glyph(out, &big, lay->letters, lay->caps[1], swap ? "cross" : "circle", 365 + 22, 305 + 22, 305 + 22, swap ? 10 : 11, RED);
    img_alpha_composite(out, red_cap, 425, 305);
    face_glyph(out, &big, lay->letters, lay->caps[2], "triangle", 425 + 22, 305 + 22, 305 + 23, 11, RED);
    img_free(red_cap);

    Image* sq = square_cap(src, 380, 392, 454, 437);
    composite_owned(out, blank_cap(sq, 0, 0, 45, 45), 365, 392);
    img_free(sq);
    face_glyph(out, &big, lay->letters, lay->caps[3], "square", 365 + 22, 392 + 22, 392 + 22, 10, YELLOW_LABEL);
    draw_text_la(out, &small, 425, 401, "hold", YELLOW_LABEL);

    composite_owned(out, blank_cap(src, 478, 472, 522, 516), 365, 472);
    draw_text_centered(out, lay->ff_big ? &big : &small, 365 + 22, 472 + 22, lay->ff_cap, PURPLE);
    draw_text_la(out, &small, 425, 481, "hold", PURPLE);

    double scale = art->w / lay->view_w;
    if (swap) swap_buttons(art, lay, scale);
    img_alpha_composite(out, art, lay->pos_x, lay->pos_y);
    img_free(art);

    Image* lines = img_new(out->w * SS, out->h * SS);
    Pt ends[ACT_COUNT];
    const Line* by_act[ACT_COUNT];
    for (int i = 0; i < ACT_COUNT; i++) {
        const Line* ln = &lay->lines[i];
        Pt c = at(lay, scale, ln->x, ln->y);
        Pt end = {ln->rel_x ? c.x + ln->ex : ln->ex, ln->rel_y ? c.y + ln->ey : ln->ey};
        Pt start = ln->edge ? edge_pt(lay, scale, ln->x, ln->y, end) : c;
        Action act = swapped(ln->act, swap);
        tapered(lines, start, end, *line_color(act));
        ends[act] = end;
        by_act[act] = ln;
    }

    img_premultiply(lines);
    Image* lsmall = img_resize(lines, out->w, out->h, FILTER_LANCZOS);
    img_free(lines);
    unpremultiply(lsmall);
    img_alpha_composite(out, lsmall, 0, 0);
    img_free(lsmall);

    const int ff_erase[4] = {0, 28, 16, 45};
    for (int a = 0; a < ACT_COUNT; a++) {
        const Line* ln = by_act[a];
        label(out, src, LABEL_BOX[a], lay->label_scale, ends[a].x + ln->lx, ends[a].y + ln->ly, ln->anchor_x, ln->anchor_y, a == ACT_FF ? ff_erase : NULL);
    }
    img_free(src);
    return out;
}
