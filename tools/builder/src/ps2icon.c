#include "ps2icon.h"

#include <string.h>

static void put16(uint8_t* p, uint16_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void put32(uint8_t* p, uint32_t v) {
    put16(p, (uint16_t)v);
    put16(p + 2, (uint16_t)(v >> 16));
}

static void putf(uint8_t* p, float f) {
    uint32_t v;
    memcpy(&v, &f, 4);
    put32(p, v);
}

static uint16_t sjis(char c) {
    if (c >= 'A' && c <= 'Z') return (uint16_t)(0x8260 + (c - 'A'));
    if (c >= 'a' && c <= 'z') return (uint16_t)(0x8281 + (c - 'a'));
    if (c >= '0' && c <= '9') return (uint16_t)(0x824F + (c - '0'));
    if (c == '-') return 0x817C;
    if (c == ':') return 0x8146;
    if (c == '.') return 0x8144;
    if (c == '\'') return 0x8166;
    return 0x8140;
}

void ps2_icon_sys(const char* title, Buf* out) {
    uint8_t s[964];
    memset(s, 0, sizeof(s));
    memcpy(s, "PS2D", 4);
    put32(s + 12, 0x60);
    static const uint32_t bg[4][3] = {{70, 4, 8}, {70, 4, 8}, {6, 0, 2}, {6, 0, 2}};
    for (int i = 0; i < 4; i++) {
        for (int c = 0; c < 3; c++) put32(s + 16 + i * 16 + c * 4, bg[i][c]);
    }
    static const float dir[3][3] = {{0.5f, 0.5f, 0.5f}, {0.0f, -0.4f, -0.1f}, {-0.5f, -0.5f, 0.5f}};
    static const float col[3][3] = {{0.5f, 0.5f, 0.5f}, {0.3f, 0.3f, 0.3f}, {0.2f, 0.2f, 0.2f}};
    for (int i = 0; i < 3; i++) {
        for (int c = 0; c < 3; c++) {
            putf(s + 80 + i * 16 + c * 4, dir[i][c]);
            putf(s + 128 + i * 16 + c * 4, col[i][c]);
        }
    }
    for (int c = 0; c < 3; c++) putf(s + 176 + c * 4, 0.75f);
    const char* split = strstr(title, " of ");
    size_t first = split ? (size_t)(split - title) + 3 : strlen(title);
    size_t pos = 0, nl = 0;
    for (size_t i = 0; title[i] && pos + 2 <= 66; i++) {
        if (i == first) {
            nl = pos;
            if (title[i] == ' ') continue;
        }
        uint16_t v = sjis(title[i]);
        s[192 + pos] = (uint8_t)(v >> 8);
        s[192 + pos + 1] = (uint8_t)v;
        pos += 2;
    }
    put16(s + 6, (uint16_t)nl);
    for (int k = 0; k < 3; k++) memcpy(s + 260 + k * 64, "icon.icn", 8);
    buf_put(out, s, sizeof(s));
}

static void vertex(Buf* out, int x, int y, int u, int v) {
    uint8_t p[24];
    put16(p, (uint16_t)(int16_t)x);
    put16(p + 2, (uint16_t)(int16_t)y);
    put16(p + 4, 0);
    put16(p + 6, 0x400);
    put16(p + 8, 0);
    put16(p + 10, 0);
    put16(p + 12, (uint16_t)(int16_t)-4096);
    put16(p + 14, 0x400);
    put16(p + 16, (uint16_t)(int16_t)u);
    put16(p + 18, (uint16_t)(int16_t)v);
    p[20] = 0x80;
    p[21] = 0x80;
    p[22] = 0x80;
    p[23] = 0x80;
    buf_put(out, p, sizeof(p));
}

void ps2_icon_icn(const Image* art, Buf* out) {
    uint8_t h[20];
    put32(h, 0x010000);
    put32(h + 4, 1);
    put32(h + 8, 0x07);
    putf(h + 12, 1.0f);
    put32(h + 16, 6);
    buf_put(out, h, sizeof(h));
    const int x0 = -6554, x1 = 6554, y0 = -16794, y1 = -3686;
    vertex(out, x0, y0, 0, 0);
    vertex(out, x1, y0, 4096, 0);
    vertex(out, x1, y1, 4096, 4096);
    vertex(out, x0, y0, 0, 0);
    vertex(out, x1, y1, 4096, 4096);
    vertex(out, x0, y1, 0, 4096);
    uint8_t anim[36];
    memset(anim, 0, sizeof(anim));
    put32(anim, 1);
    put32(anim + 4, 1);
    put32(anim + 16, 1);
    put32(anim + 24, 1);
    putf(anim + 32, 1.0f);
    buf_put(out, anim, sizeof(anim));
    Image* tex = NULL;
    if (art) tex = img_resize(art, 128, 128, FILTER_LANCZOS);
    for (int y = 0; y < 128; y++) {
        for (int x = 0; x < 128; x++) {
            int r = 40, g = 0, b = 4, a = 255;
            if (tex) {
                const uint8_t* p = tex->px + ((size_t)y * 128 + x) * 4;
                a = p[3];
                r = p[0] * a / 255 + 6 * (255 - a) / 255;
                g = p[1] * a / 255;
                b = p[2] * a / 255 + 2 * (255 - a) / 255;
            }
            uint16_t v = (uint16_t)((r >> 3) | ((g >> 3) << 5) | ((b >> 3) << 10) | 0x8000);
            uint8_t q[2] = {(uint8_t)v, (uint8_t)(v >> 8)};
            buf_put(out, q, 2);
        }
    }
    img_free(tex);
}
