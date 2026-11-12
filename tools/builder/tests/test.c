#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "atrac3.h"
#include "cia.h"
#include "crypto.h"
#include "cwav.h"
#include "disc.h"
#include "image.h"
#include "iso.h"
#include "jsmini.h"
#include "miniz.h"
#include "node.h"
#include "pack.h"
#include "pkg.h"
#include "ps2icon.h"
#include "psxtex.h"
#include "target.h"
#include "util.h"
#include "vpk.h"
#include "wuhb.h"

static int g_fail = 0, g_run = 0;

#define CHECK(cond)                                                      \
    do {                                                                 \
        g_run++;                                                         \
        if (!(cond)) {                                                   \
            g_fail++;                                                    \
            fprintf(stderr, "fail %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                \
    } while (0)

static void to_json(const Node* n, Buf* b) {
    char tmp[64];
    switch (n->type) {
    case N_NULL: buf_put(b, "null", 4); break;
    case N_TRUE: buf_put(b, "true", 4); break;
    case N_FALSE: buf_put(b, "false", 5); break;
    case N_NUM:
        snprintf(tmp, sizeof(tmp), "%g", n->num);
        buf_put(b, tmp, strlen(tmp));
        break;
    case N_STR:
        buf_u8(b, '"');
        buf_put(b, n->str, n->len);
        buf_u8(b, '"');
        break;
    case N_BYTES: buf_put(b, "bytes", 5); break;
    case N_ARR:
    case N_CMD:
        buf_u8(b, '[');
        for (size_t i = 0; i < n->count; i++) {
            if (i) buf_u8(b, ',');
            to_json(n->items[i], b);
        }
        buf_u8(b, ']');
        break;
    case N_OBJ:
        buf_u8(b, '{');
        for (size_t i = 0; i < n->count; i++) {
            if (i) buf_u8(b, ',');
            buf_u8(b, '"');
            buf_put(b, n->keys[i], strlen(n->keys[i]));
            buf_put(b, "\":", 2);
            to_json(n->items[i], b);
        }
        buf_u8(b, '}');
        break;
    }
}

static int script_is(const char* src, int expr, const char* want) {
    char err[128];
    Node* n = expr ? js_compile_expr(src, err, sizeof(err)) : js_compile_script(src, err, sizeof(err));
    if (!n) {
        fprintf(stderr, "compile failed: %s (%s)\n", src, err);
        return 0;
    }
    Buf b = {0};
    to_json(n, &b);
    buf_u8(&b, 0);
    int ok = !strcmp((const char*)b.data, want);
    if (!ok) fprintf(stderr, "got %s\nwant %s\n", (const char*)b.data, want);
    buf_free(&b);
    node_free(n);
    return ok;
}

static int key_for(const char* url) {
    const char* s = strrchr(url, '/');
    s = s ? s + 1 : url;
    const char* dot = strrchr(s, '.');
    size_t n = dot && dot > s ? (size_t)(dot - s) : strlen(s);
    uint32_t mask = 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        mask = (mask << 1) ^ (uint32_t)(c >= 'a' && c <= 'z' ? c - 32 : c);
    }
    int key = (int)(((int64_t)(int32_t)mask + 1) % 256);
    return key < 0 ? key + 256 : key;
}

static void encrypt(uint8_t* out, const uint8_t* plain, size_t n, const char* url, int count) {
    memcpy(out, "TCOAAL", 6);
    out[6] = (uint8_t)count;
    memcpy(out + 7, plain, n);
    size_t limit = count ? (size_t)count : n;
    int key = key_for(url);
    for (size_t i = 0; i < limit && i < n; i++) {
        uint8_t c = (uint8_t)(plain[i] ^ key);
        out[7 + i] = c;
        key = ((key << 1) ^ c) & 255;
    }
}

static void test_crypto(void) {
    char* h = hash_path("data/System.json");
    CHECK(!strcmp(h, "data/be1a37535e921f91"));
    free(h);
    h = hash_path("img/faces/Andy[BUST].png");
    CHECK(strlen(h) > 6 && !strcmp(h + strlen(h) - 6, "[BUST]"));
    free(h);
    h = hash_path("img/characters/!Door.png");
    CHECK(strrchr(h, '/')[1] == '!');
    free(h);
    uint8_t plain[768];
    for (int i = 0; i < 768; i++) plain[i] = (uint8_t)i;
    const char* url = "img/pictures/0123456789abcdef";
    for (int pass = 0; pass < 2; pass++) {
        uint8_t enc[775];
        encrypt(enc, plain, 768, url, pass ? 100 : 0);
        size_t len = 775;
        asset_decrypt(enc, &len, url);
        CHECK(len == 768 && !memcmp(enc, plain, 768));
    }
    uint8_t txt[2] = {'{', '}'};
    size_t tl = 2;
    asset_decrypt(txt, &tl, "data/x");
    CHECK(tl == 2 && txt[0] == '{');
}

static void test_json_vbin(void) {
    const char* src = "{\"a\":[1,-2,3.5,null,true,false,\"t\\u00e9xt\",{\"k\":\"v\"}],\"big\":1099511627776}";
    Node* n = json_parse(src, strlen(src), NULL, 0);
    CHECK(n != NULL);
    CHECK(!strcmp(node_strv(node_at(node_get(n, "a"), 6)), "t\xC3\xA9xt"));
    Buf b = {0};
    vbin_dump(n, &b);
    CHECK(b.len > 12 && !memcmp(b.data, "VBIN", 4));
    static const uint8_t big[] = {4, 0, 0, 0, 0, 0, 0, 0x70, 0x42};
    int found = 0;
    for (size_t i = 0; i + sizeof(big) <= b.len; i++) {
        if (!memcmp(b.data + i, big, sizeof(big))) found = 1;
    }
    CHECK(found);
    static const uint8_t ints[] = {6, 8, 3, 2, 3, 3, 4};
    found = 0;
    for (size_t i = 0; i + sizeof(ints) <= b.len; i++) {
        if (!memcmp(b.data + i, ints, sizeof(ints))) found = 1;
    }
    CHECK(found);
    buf_free(&b);
    node_free(n);
    CHECK(json_parse("{\"a\":", 5, NULL, 0) == NULL);
}

static uint32_t rd32(const uint8_t* p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | p[1] << 8); }

static const uint8_t* vbn_at(const uint8_t* slot) { return slot + (int32_t)rd32(slot); }

static void test_json_vbn2(void) {
    target_set(target_get(TARGET_PS1));
    const char* src = "{\"a\":[1,-2,3.5,null,true,false,\"t\\u00e9xt\",{\"a\":\"t\\u00e9xt\"}],\"big\":1099511627776,\"i\":2000000000}";
    Node* n = json_parse(src, strlen(src), NULL, 0);
    CHECK(n != NULL);
    Buf b = {0};
    vbin_dump(n, &b);
    CHECK(b.len % 4 == 0 && !memcmp(b.data, "VBN2", 4) && rd32(b.data + 4) == b.len);
    const uint8_t* root = vbn_at(b.data + 8);
    CHECK(rd32(root) == (3u << 3 | 3));
    const uint8_t* key_a = vbn_at(root + 4);
    CHECK(rd32(key_a) == (1u << 3 | 1) && !strcmp((const char*)key_a + 4, "a"));
    const uint8_t* arr = vbn_at(root + 8);
    CHECK(rd32(arr) == (8u << 3 | 2));
    CHECK(rd32(arr + 4) == 3 && rd32(arr + 8) == 0xFFFFFFFDu && rd32(arr + 16) == 0 && rd32(arr + 20) == 6 && rd32(arr + 24) == 2);
    const uint8_t* num = vbn_at(arr + 12);
    double d;
    uint64_t bits = (uint64_t)rd32(num + 4) | (uint64_t)rd32(num + 8) << 32;
    memcpy(&d, &bits, 8);
    CHECK(rd32(num) == 4 && d == 3.5);
    const uint8_t* text = vbn_at(arr + 28);
    CHECK(rd32(text) == (5u << 3 | 1) && !strcmp((const char*)text + 4, "t\xC3\xA9xt"));
    const uint8_t* inner = vbn_at(arr + 32);
    CHECK(vbn_at(inner + 4) == key_a && vbn_at(inner + 8) == text);
    const uint8_t* big = vbn_at(root + 16);
    CHECK(rd32(big) == 4);
    const uint8_t* i32 = vbn_at(root + 24);
    CHECK(rd32(i32) == 5 && rd32(i32 + 4) == 2000000000u);
    buf_free(&b);
    node_free(n);
    target_set(target_get(TARGET_PSP));
}

static void test_scripts(void) {
    CHECK(script_is("$gameSelfSwitches.setValue([12, 3, 'A'], true);", 0,
                    "[[\"call\",\"selfSwitch.set\",[[\"num\",12],[\"num\",3],[\"str\",\"A\"],[\"bool\",true]]]]"));
    CHECK(script_is("$bust(1).loadBitmap('face', 'x')\n$bust(1).fadeIn(1)", 0,
                    "[[\"call\",\"bust.loadBitmap\",[[\"num\",1],[\"str\",\"face\"],[\"str\",\"x\"]]],[\"call\",\"bust.fadeIn\",[[\"num\",1],[\"num\",1]]]]"));
    CHECK(script_is("$gameMap.event(3).x === 5 && $gameMap.event(3).y === 7", 1,
                    "[\"bin\",\"&&\",[\"bin\",\"===\",[\"call\",\"event.x\",[[\"num\",3]]],[\"num\",5]],[\"bin\",\"===\",[\"call\",\"event.y\",[[\"num\",3]]],[\"num\",7]]]"));
    CHECK(script_is("if ($gameVariables._backup) $gameVariables._data = 1;", 0, "[[\"call\",\"restoreBackup\",[]]]"));
    CHECK(script_is("enabled:1==on", 0, "[[\"call\",\"nop\",[]]]"));
    char err[128];
    Node* bad = js_compile_script("SceneManager.goto(Scene_Battle)", err, sizeof(err));
    CHECK(bad == NULL);
    bad = js_compile_script("globalTagAdd('x'); $gameVariables._data = 2", err, sizeof(err));
    CHECK(bad == NULL);
}

static uint32_t rd32be(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

static Image* noise_image(void) {
    Image* img = img_new(70, 37);
    uint32_t s = 1;
    for (int i = 0; i < 70 * 37 * 4; i++) {
        s = s * 1103515245u + 12345u;
        img->px[i] = (uint8_t)(s >> 16);
    }
    for (int i = 0; i < 70 * 37; i++) {
        uint8_t* p = img->px + i * 4;
        for (int c = 0; c < 3; c++) p[c] = (uint8_t)(p[c] * p[3] / 255);
    }
    return img;
}

static void test_texture_alpha_flags(void) {
    target_set(target_get(TARGET_PSP));
    Image* img = img_new(16, 8);
    for (int i = 0; i < 16 * 8; i++) img->px[i * 4 + 3] = 255;
    Buf t = {0};
    CHECK(tex_encode(img, 16, 8, &t) && (t.data[6] & (8 | 16)) == 8);
    buf_free(&t);
    img->px[3] = 0;
    CHECK(tex_encode(img, 16, 8, &t) && (t.data[6] & (8 | 16)) == 16);
    buf_free(&t);
    img->px[7] = 128;
    CHECK(tex_encode(img, 16, 8, &t) && (t.data[6] & (8 | 16)) == 0);
    buf_free(&t);
    img_free(img);
}

static void test_texture_psp(void) {
    target_set(target_get(TARGET_PSP));
    Image* img = noise_image();
    Buf t = {0};
    CHECK(tex_encode(img, 70, 37, &t));
    CHECK(t.len > 44 && !memcmp(t.data, "TTEX", 4));
    CHECK(t.data[4] == TEX_VERSION && t.data[5] == 0 && (t.data[6] & 7) == 1 && rd16(t.data + 8) == 70 && rd16(t.data + 10) == 37 && rd16(t.data + 16) == 1);
    const uint8_t* e = t.data + 20;
    int stride = rd16(e + 8), rows = rd16(e + 10);
    uint32_t off = rd32(e + 12), csize = rd32(e + 16), rsize = rd32(e + 20);
    CHECK(stride % 16 == 0 && rows % 8 == 0);
    uint8_t* raw = xmalloc(rsize);
    mz_ulong rl = rsize;
    CHECK(mz_uncompress(raw, &rl, t.data + off, csize) == MZ_OK && rl == rsize);
    int bw = stride * 4, ok = 1;
    for (int y = 0; y < 37 && ok; y++) {
        for (int x = 0; x < 70 * 4 && ok; x++) {
            size_t sw = ((size_t)(y / 8) * (size_t)(bw / 16) + (size_t)(x / 16)) * 128 + (size_t)(y % 8) * 16 + (size_t)(x % 16);
            if (raw[sw] != img->px[y * 70 * 4 + x]) ok = 0;
        }
    }
    CHECK(ok);
    free(raw);
    buf_free(&t);
    img_free(img);
    Image* big = img_new(700, 600);
    Buf tb = {0};
    CHECK(tex_encode(big, 700, 600, &tb));
    CHECK(rd16(tb.data + 16) == 4);
    buf_free(&tb);
    img_free(big);
}

static void test_texture_3ds(void) {
    target_set(target_get(TARGET_3DS));
    Image* img = noise_image();
    Buf t = {0};
    CHECK(tex_encode(img, 70, 37, &t));
    CHECK(t.len > 44 && !memcmp(t.data, "TTEX", 4));
    CHECK(t.data[4] == 2 && t.data[5] == 0 && (t.data[6] & 7) == 1);
    CHECK(rd16(t.data + 8) == 70 && rd16(t.data + 10) == 37 && rd16(t.data + 16) == 1);
    const uint8_t* e = t.data + 20;
    int stride = rd16(e + 8), rows = rd16(e + 10);
    uint32_t off = rd32(e + 12), csize = rd32(e + 16), rsize = rd32(e + 20);
    CHECK(stride == 128 && rows == 48 && rsize == 128u * 48u * 4u);
    uint8_t* raw = xmalloc(rsize);
    mz_ulong rl = rsize;
    CHECK(mz_uncompress(raw, &rl, t.data + off, csize) == MZ_OK && rl == rsize);
    int ok = 1;
    for (int y = 0; y < rows && ok; y++) {
        for (int x = 0; x < stride && ok; x++) {
            int m = (x & 1) | ((y & 1) << 1) | ((x & 2) << 1) | ((y & 2) << 2) | ((x & 4) << 2) | ((y & 4) << 3);
            const uint8_t* d = raw + (((size_t)(y / 8) * (size_t)(stride / 8) + (size_t)(x / 8)) * 64 + (size_t)m) * 4;
            int sx = x < 70 ? x : 69, sy = y < 37 ? y : 36;
            const uint8_t* p = img->px + ((size_t)sy * 70 + (size_t)sx) * 4;
            if (d[0] != p[3] || d[1] != p[2] || d[2] != p[1] || d[3] != p[0]) ok = 0;
        }
    }
    CHECK(ok);
    free(raw);
    buf_free(&t);
    img_free(img);
    Image* big = img_new(700, 600);
    Buf tb = {0};
    CHECK(tex_encode(big, 700, 600, &tb));
    CHECK(rd16(tb.data + 16) == 4);
    buf_free(&tb);
    img_free(big);
}

static void test_texture_ps2(void) {
    target_set(target_get(TARGET_PS2));
    Image* img = noise_image();
    Buf t = {0};
    CHECK(tex_encode(img, 70, 37, &t));
    CHECK(t.len > 44 && !memcmp(t.data, "TTEX", 4));
    CHECK(t.data[4] == TEX_VERSION && t.data[5] == 0 && (t.data[6] & 7) == 0);
    CHECK(rd16(t.data + 8) == 70 && rd16(t.data + 10) == 37 && rd16(t.data + 16) == 1);
    const uint8_t* e = t.data + 20;
    int stride = rd16(e + 8), rows = rd16(e + 10);
    uint32_t off = rd32(e + 12), csize = rd32(e + 16), rsize = rd32(e + 20);
    CHECK(stride % 4 == 0 && stride >= 70 && rows == 37 && rsize == (uint32_t)stride * 4 * 37);
    uint8_t* raw = xmalloc(rsize);
    mz_ulong rl = rsize;
    CHECK(mz_uncompress(raw, &rl, t.data + off, csize) == MZ_OK && rl == rsize);
    int ok = 1;
    for (int y = 0; y < 37 && ok; y++) {
        for (int x = 0; x < 70 && ok; x++) {
            const uint8_t* src = img->px + ((size_t)y * 70 + x) * 4;
            const uint8_t* got = raw + ((size_t)y * stride + x) * 4;
            int a = src[3];
            if (got[3] != (a * 128 + 127) / 255) ok = 0;
            for (int c = 0; c < 3 && a; c++) {
                int want = (src[c] * 255 + a / 2) / a;
                if (want > 255) want = 255;
                if (got[c] != want) ok = 0;
            }
        }
        const uint8_t* last = raw + ((size_t)y * stride + 69) * 4;
        for (int x = 70; x < stride; x++) {
            if (memcmp(raw + ((size_t)y * stride + x) * 4, last, 4) != 0) ok = 0;
        }
    }
    CHECK(ok);
    free(raw);
    buf_free(&t);
    img_free(img);
    Image* big = img_new(700, 600);
    Buf tb = {0};
    CHECK(tex_encode(big, 700, 600, &tb));
    CHECK(rd16(tb.data + 16) == 4);
    buf_free(&tb);
    img_free(big);
}

static void test_texture_ps3(void) {
    target_set(target_get(TARGET_PS3));
    Image* img = noise_image();
    Buf t = {0};
    CHECK(tex_encode(img, 70, 37, &t));
    CHECK(t.len > 44 && !memcmp(t.data, "TTEX", 4));
    CHECK(t.data[4] == TEX_VERSION && t.data[5] == 5 && (t.data[6] & ~(4 | 8 | 16)) == 0 && rd16(t.data + 8) == 70 && rd16(t.data + 10) == 37 && rd16(t.data + 16) == 1);
    const uint8_t* e = t.data + 20;
    int stride = rd16(e + 8), rows = rd16(e + 10);
    uint32_t off = rd32(e + 12), csize = rd32(e + 16), rsize = rd32(e + 20);
    CHECK(stride % 16 == 0 && stride >= 70 && rows == 37 && rsize == (uint32_t)(stride * 4 * rows));
    uint8_t* raw = xmalloc(rsize);
    mz_ulong rl = rsize;
    CHECK(mz_uncompress(raw, &rl, t.data + off, csize) == MZ_OK && rl == rsize);
    if (t.data[6] & 4) {
        for (int y = 1; y < rows; y++) {
            for (int i = 0; i < stride * 4; i++) raw[(size_t)y * stride * 4 + i] = (uint8_t)(raw[(size_t)y * stride * 4 + i] + raw[(size_t)(y - 1) * stride * 4 + i]);
        }
    }
    int ok = 1;
    for (int y = 0; y < 37 && ok; y++) {
        for (int x = 0; x < 70 && ok; x++) {
            const uint8_t* d = raw + ((size_t)y * stride + x) * 4;
            const uint8_t* p = img->px + ((size_t)y * 70 + x) * 4;
            if (d[0] != p[3] || d[1] != p[0] || d[2] != p[1] || d[3] != p[2]) ok = 0;
        }
    }
    CHECK(ok);
    free(raw);
    buf_free(&t);
    img_free(img);
    Image* grad = img_new(64, 64);
    for (int y = 0; y < 64; y++) {
        for (int x = 0; x < 64; x++) {
            uint8_t* p = grad->px + ((size_t)y * 64 + x) * 4;
            p[0] = (uint8_t)(x * 3 + y);
            p[1] = (uint8_t)(x * 5 + y * 2);
            p[2] = (uint8_t)(x * 7 + y * 3);
            p[3] = 255;
        }
    }
    Buf tg = {0};
    CHECK(tex_encode(grad, 64, 64, &tg) && (tg.data[6] & 4));
    buf_free(&tg);
    img_free(grad);
    Image* wide = img_new(5000, 10);
    Buf tb = {0};
    CHECK(tex_encode(wide, 5000, 10, &tb));
    CHECK(rd16(tb.data + 16) == 2 && rd16(tb.data + 20 + 4) == 4096 && rd16(tb.data + 44) == 4096 && rd16(tb.data + 44 + 4) == 904);
    buf_free(&tb);
    img_free(wide);
}

static void test_texture_linear(void) {
    const TargetId ids[2] = {TARGET_VITA, TARGET_WIIU};
    const int max[2] = {2048, 4096};
    for (int k = 0; k < 2; k++) {
        target_set(target_get(ids[k]));
        Image* img = noise_image();
        Buf t = {0};
        CHECK(tex_encode(img, 70, 37, &t));
        CHECK(t.len > 44 && !memcmp(t.data, "TTEX", 4));
        CHECK(t.data[4] == TEX_VERSION && t.data[5] == 0 && (t.data[6] & ~(4 | 8 | 16)) == 0 && rd16(t.data + 8) == 70 && rd16(t.data + 10) == 37 && rd16(t.data + 16) == 1);
        const uint8_t* e = t.data + 20;
        int stride = rd16(e + 8), rows = rd16(e + 10);
        uint32_t off = rd32(e + 12), csize = rd32(e + 16), rsize = rd32(e + 20);
        CHECK(stride == 80 && rows == 37 && rsize == (uint32_t)(stride * 4 * rows));
        uint8_t* raw = xmalloc(rsize);
        mz_ulong rl = rsize;
        CHECK(mz_uncompress(raw, &rl, t.data + off, csize) == MZ_OK && rl == rsize);
        if (t.data[6] & 4) {
            for (int y = 1; y < rows; y++) {
                for (int i = 0; i < stride * 4; i++) raw[(size_t)y * stride * 4 + i] = (uint8_t)(raw[(size_t)y * stride * 4 + i] + raw[(size_t)(y - 1) * stride * 4 + i]);
            }
        }
        int ok = 1;
        for (int y = 0; y < rows && ok; y++) {
            for (int x = 0; x < stride && ok; x++) {
                int sx = x < 70 ? x : 69;
                if (memcmp(raw + ((size_t)y * stride + x) * 4, img->px + ((size_t)y * 70 + sx) * 4, 4) != 0) ok = 0;
            }
        }
        CHECK(ok);
        free(raw);
        buf_free(&t);
        img_free(img);
        Image* wide = img_new(5000, 10);
        Buf tb = {0};
        CHECK(tex_encode(wide, 5000, 10, &tb));
        CHECK(rd16(tb.data + 16) == (5000 + max[k] - 1) / max[k] && rd16(tb.data + 20 + 4) == max[k]);
        buf_free(&tb);
        img_free(wide);
    }
}

static void test_texture_wii(void) {
    target_set(target_get(TARGET_WII));
    Image* img = noise_image();
    Buf t = {0};
    CHECK(tex_encode(img, 70, 37, &t));
    CHECK(t.len > 44 && !memcmp(t.data, "TTEX", 4));
    CHECK(t.data[4] == TEX_VERSION && t.data[5] == 0 && (t.data[6] & 7) == 1 && rd16(t.data + 8) == 70 && rd16(t.data + 10) == 37 && rd16(t.data + 16) == 1);
    const uint8_t* e = t.data + 20;
    int stride = rd16(e + 8), rows = rd16(e + 10);
    uint32_t off = rd32(e + 12), csize = rd32(e + 16), rsize = rd32(e + 20);
    CHECK(stride == 72 && rows == 40 && rsize == (uint32_t)(stride * rows * 4));
    uint8_t* raw = xmalloc(rsize);
    mz_ulong rl = rsize;
    CHECK(mz_uncompress(raw, &rl, t.data + off, csize) == MZ_OK && rl == rsize);
    int ok = 1;
    for (int y = 0; y < rows && ok; y++) {
        for (int x = 0; x < stride && ok; x++) {
            const uint8_t* tile = raw + ((size_t)(y / 4) * (size_t)(stride / 4) + (size_t)(x / 4)) * 64;
            size_t at = (size_t)((y % 4) * 4 + x % 4) * 2;
            int sx = x < 70 ? x : 69, sy = y < 37 ? y : 36;
            const uint8_t* p = img->px + ((size_t)sy * 70 + (size_t)sx) * 4;
            if (tile[at] != p[3] || tile[at + 1] != p[0] || tile[32 + at] != p[1] || tile[32 + at + 1] != p[2]) ok = 0;
        }
    }
    CHECK(ok);
    free(raw);
    buf_free(&t);
    img_free(img);
    Image* big = img_new(1500, 300);
    Buf tb = {0};
    CHECK(tex_encode(big, 1500, 300, &tb));
    CHECK(rd16(tb.data + 16) == 2 && rd16(tb.data + 20 + 4) == 1024);
    buf_free(&tb);
    img_free(big);
    CHECK(target_get(TARGET_XENON)->layout == target_get(TARGET_PS3)->layout);
}

static void test_png_indexed(void) {
    Image* img = img_new(40, 30);
    for (int y = 0; y < 30; y++) {
        for (int x = 0; x < 40; x++) {
            uint8_t* p = img->px + ((size_t)y * 40 + x) * 4;
            p[0] = (uint8_t)(x * 6);
            p[1] = (uint8_t)(y * 8);
            p[2] = (uint8_t)((x + y) * 3);
            p[3] = 255;
        }
    }
    Buf png = {0};
    CHECK(png_encode_indexed(img, &png));
    CHECK(png.len > 33 && png.data[24] == 8 && png.data[25] == 3);
    Image* back = img_load_png(png.data, png.len);
    CHECK(back && back->w == 40 && back->h == 30);
    int worst = 0;
    for (int i = 0; back && i < 40 * 30 * 4; i++) {
        int d = abs((int)back->px[i] - (int)img->px[i]);
        if (d > worst) worst = d;
    }
    CHECK(worst <= 24);
    img_free(back);
    img_free(img);
    buf_free(&png);
}

static int zip_entry(const Buf* zip, const char* name, Buf* out) {
    mz_zip_archive z;
    memset(&z, 0, sizeof(z));
    if (!mz_zip_reader_init_mem(&z, zip->data, zip->len, 0)) return 0;
    size_t n = 0;
    void* p = mz_zip_reader_extract_file_to_heap(&z, name, &n, 0);
    mz_zip_reader_end(&z);
    if (!p) return 0;
    buf_put(out, p, n);
    mz_free(p);
    return 1;
}

static void test_vpk(void) {
    Vpk v;
    memset(&v, 0, sizeof(v));
    v.eboot = (const uint8_t*)"SCE\0old";
    v.eboot_len = 7;
    v.sfo = (const uint8_t*)"\0PSFsfo";
    v.sfo_len = 7;
    buf_put(&v.icon0, "icon", 4);
    buf_put(&v.bg, "bg", 2);
    buf_put(&v.startup, "gate", 4);
    Buf zip = {0};
    CHECK(vpk_build(&v, &zip));
    CHECK(vpk_is_vpk(zip.data, zip.len));
    Buf a = {0}, b = {0}, c = {0};
    CHECK(zip_entry(&zip, "eboot.bin", &a) && a.len == 7 && !memcmp(a.data, "SCE\0old", 7));
    CHECK(zip_entry(&zip, "sce_sys/livearea/contents/template.xml", &b) && b.len > 0);
    CHECK(zip_entry(&zip, "sce_sys/icon0.png", &c) && c.len == 4);
    Buf up = {0}, e = {0}, s = {0};
    CHECK(vpk_replace_eboot(zip.data, zip.len, (const uint8_t*)"SCE\0new!", 8, &up));
    CHECK(zip_entry(&up, "eboot.bin", &e) && e.len == 8 && !memcmp(e.data, "SCE\0new!", 8));
    CHECK(zip_entry(&up, "sce_sys/param.sfo", &s) && s.len == 7 && !memcmp(s.data, "\0PSFsfo", 7));
    CHECK(!vpk_is_vpk((const uint8_t*)"PK\3\4junk", 8));
    buf_free(&v.icon0);
    buf_free(&v.bg);
    buf_free(&v.startup);
    buf_free(&zip);
    buf_free(&a);
    buf_free(&b);
    buf_free(&c);
    buf_free(&up);
    buf_free(&e);
    buf_free(&s);
}

static int wuhb_file(const Buf* w, const char* name, const uint8_t** data, uint64_t* len) {
    uint64_t ft = (uint64_t)rd32be(w->data + 0x38) << 32 | rd32be(w->data + 0x3C), fl = (uint64_t)rd32be(w->data + 0x40) << 32 | rd32be(w->data + 0x44);
    uint64_t part = (uint64_t)rd32be(w->data + 0x48) << 32 | rd32be(w->data + 0x4C);
    for (uint64_t at = 0; at + 0x20 <= fl;) {
        const uint8_t* e = w->data + ft + at;
        uint32_t n = rd32be(e + 0x1C);
        if (n == strlen(name) && !memcmp(e + 0x20, name, n)) {
            *data = w->data + part + ((uint64_t)rd32be(e + 8) << 32 | rd32be(e + 12));
            *len = (uint64_t)rd32be(e + 16) << 32 | rd32be(e + 20);
            return 1;
        }
        at += 0x20 + ((n + 3) & ~3u);
    }
    return 0;
}

static void test_wuhb(void) {
    Image* img = img_new(4, 2);
    memset(img->px, 200, 4 * 2 * 4);
    Wuhb w;
    memset(&w, 0, sizeof(w));
    w.rpx = (const uint8_t*)"rpx-one";
    w.rpx_len = 7;
    CHECK(wuhb_tga_gz(img, &w.icon));
    CHECK(w.icon.len > 18 && w.icon.data[0] == 0x1F && w.icon.data[1] == 0x8B);
    Buf out = {0};
    wuhb_build(&w, &out);
    CHECK(wuhb_is_wuhb(out.data, out.len));
    const uint8_t* d;
    uint64_t n;
    CHECK(wuhb_file(&out, "root.rpx", &d, &n) && n == 7 && !memcmp(d, "rpx-one", 7));
    CHECK(wuhb_file(&out, "meta.ini", &d, &n) && n > 6 && !memcmp(d, "[menu]", 6));
    CHECK(wuhb_file(&out, "iconTex.tga.gz", &d, &n) && n == w.icon.len && !memcmp(d, w.icon.data, n));
    Buf up = {0};
    CHECK(wuhb_replace_rpx(out.data, out.len, (const uint8_t*)"rpx-second", 10, &up));
    CHECK(wuhb_file(&up, "root.rpx", &d, &n) && n == 10 && !memcmp(d, "rpx-second", 10));
    CHECK(wuhb_file(&up, "iconTex.tga.gz", &d, &n) && n == w.icon.len && !memcmp(d, w.icon.data, n));
    CHECK(!wuhb_is_wuhb((const uint8_t*)"WUHB", 4));
    buf_free(&w.icon);
    buf_free(&out);
    buf_free(&up);
    img_free(img);
}

static void test_pbp_pack(void) {
    Pbp p;
    memset(&p, 0, sizeof(p));
    sfo_build("Test", &p.part[0]);
    CHECK(!memcmp(p.part[0].data, "\0PSF", 4));
    buf_put(&p.part[6], "elf", 3);
    Pack pk = {0};
    buf_put(&pack_add(&pk, "maps/Map002.bin")->data, "bb", 2);
    buf_put(&pack_add(&pk, "game.bin")->data, "a", 1);
    pack_build(&pk, &p.part[7]);
    Buf out = {0};
    pbp_write(&p, &out);
    Pbp back;
    CHECK(pbp_read(out.data, out.len, &back));
    CHECK(back.part[0].len == p.part[0].len && !memcmp(back.part[0].data, p.part[0].data, p.part[0].len));
    CHECK(back.part[6].len == 3 && !memcmp(back.part[6].data, "elf", 3));
    const uint8_t* t = back.part[7].data;
    CHECK(!memcmp(t, "TPAK", 4) && rd32(t + 4) == PACK_VERSION && rd32(t + 8) == 2);
    const char* names = (const char*)t + 16 + 2 * 12;
    CHECK(!strcmp(names + rd32(t + 16), "game.bin") && !strcmp(names + rd32(t + 28), "maps/Map002.bin"));
    CHECK(rd32(t + 20) % 64 == 0 && rd32(t + 24) == 1 && t[rd32(t + 20)] == 'a');
    CHECK(rd32(t + 32) % 64 == 0 && rd32(t + 36) == 2 && t[rd32(t + 32)] == 'b');
    pbp_free(&p);
    pbp_free(&back);
    pack_free(&pk);
    buf_free(&out);
}

static void test_pack(void) {
    Pack pk = {0};
    buf_put(&pack_add(&pk, "maps/Map002.bin")->data, "bb", 2);
    buf_put(&pack_add(&pk, "game.bin")->data, "a", 1);
    Buf out = {0};
    pack_build(&pk, &out);
    CHECK(out.len == pack_size(&pk));
    const uint8_t* t = out.data;
    CHECK(!memcmp(t, "TPAK", 4) && rd32(t + 4) == PACK_VERSION && rd32(t + 8) == 2);
    const char* names = (const char*)t + 16 + 2 * 12;
    CHECK(!strcmp(names + rd32(t + 16), "game.bin") && !strcmp(names + rd32(t + 28), "maps/Map002.bin"));
    CHECK(rd32(t + 20) % 64 == 0 && rd32(t + 24) == 1 && t[rd32(t + 20)] == 'a');
    CHECK(rd32(t + 32) % 64 == 0 && rd32(t + 36) == 2 && t[rd32(t + 32)] == 'b');
    pack_free(&pk);
    buf_free(&out);
}

static void put32le(uint8_t* p, uint32_t v) {
    for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (i * 8));
}

static void test_3dsx(void) {
    static const uint8_t romfs_ref[] = {
        0x28, 0x00, 0x00, 0x00, 0x28, 0x00, 0x00, 0x00, 0x0c, 0x00, 0x00, 0x00, 0x34, 0x00, 0x00, 0x00, 0x18, 0x00, 0x00, 0x00, 0x4c, 0x00,
        0x00, 0x00, 0x0c, 0x00, 0x00, 0x00, 0x58, 0x00, 0x00, 0x00, 0x34, 0x00, 0x00, 0x00, 0x8c, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00, 0x00,
        0x00, 0x00, 0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x16, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0xff, 0xff, 0xff, 0xff, 0x14, 0x00, 0x00, 0x00, 0x74, 0x00, 0x63, 0x00, 0x6f, 0x00, 0x61, 0x00, 0x61, 0x00, 0x6c, 0x00,
        0x2e, 0x00, 0x70, 0x00, 0x61, 0x00, 0x6b, 0x00, 0x54, 0x50, 0x41, 0x4b, 0x2d, 0x74, 0x65, 0x73, 0x74, 0x2d, 0x64, 0x61, 0x74, 0x61,
        0x2d, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x00, 0x00,
    };
    const size_t code = 0x40, smdh = 0x36C0;
    uint8_t* engine = xcalloc(code + smdh, 1);
    memcpy(engine, "3DSX", 4);
    engine[4] = 0x2C;
    put32le(engine + 0x20, (uint32_t)code);
    put32le(engine + 0x24, (uint32_t)smdh);
    CHECK(ctr_check(engine, code + smdh));
    CHECK(!ctr_check(engine, code + smdh - 1));
    Buf pak = {0}, icon = {0}, app = {0};
    buf_put(&pak, "TPAK-test-data-1234567", 22);
    for (int i = 0; i < CTR_ICON_SIZE; i++) buf_u8(&icon, (uint8_t)(i * 7));
    CHECK(ctr_build(engine, code + smdh, icon.data, &pak, &app));
    CHECK(app.len == code + smdh + sizeof(romfs_ref));
    CHECK(rd32(app.data + 0x28) == code + smdh);
    CHECK(!memcmp(app.data + code + smdh, romfs_ref, sizeof(romfs_ref)));
    CHECK(!memcmp(app.data + code + 0x2040, icon.data, CTR_ICON_SIZE));
    Buf icon2 = {0}, pak2 = {0}, again = {0};
    CHECK(ctr_unpack(app.data, app.len, &icon2, &pak2));
    CHECK(icon2.len == CTR_ICON_SIZE && !memcmp(icon2.data, icon.data, CTR_ICON_SIZE));
    CHECK(pak2.len == pak.len && !memcmp(pak2.data, pak.data, pak.len));
    CHECK(ctr_build(app.data, app.len, NULL, &pak2, &again));
    CHECK(again.len == app.len && !memcmp(again.data, app.data, app.len));
    uint8_t px[8 * 8 * 4] = {0};
    px[(1 * 8 + 2) * 4] = 255;
    Buf enc = {0};
    ctr_icon_encode(px, 8, &enc);
    CHECK(enc.len == 128 && rd16(enc.data + 2 * 6) == 0xF800);
    uint8_t* wide = xcalloc(CTR_BANNER_W * CTR_BANNER_H * 4, 1);
    for (int i = 0; i < CTR_BANNER_W * CTR_BANNER_H; i++) wide[i * 4 + 3] = 255;
    wide[(1 * CTR_BANNER_W + 2) * 4] = 255;
    wide[(8 * CTR_BANNER_W + 0) * 4 + 2] = 255;
    Buf bnr = {0};
    ctr_banner_encode(wide, &bnr);
    CHECK(bnr.len == CTR_BANNER_SIZE && rd16(bnr.data + 2 * 6) == 0xF00F && rd16(bnr.data) == 0x000F);
    CHECK(rd16(bnr.data + 2 * 64 * (CTR_BANNER_W / 8)) == 0x00FF);
    Pack bp = {0};
    buf_put(&pack_add(&bp, "game.bin")->data, "abc", 3);
    buf_put(&pack_add(&bp, CTR_BANNER_NAME)->data, bnr.data, bnr.len);
    Buf bpak = {0}, bapp = {0};
    pack_build(&bp, &bpak);
    CHECK(ctr_build(engine, code + smdh, NULL, &bpak, &bapp));
    size_t boff = 0, blen = 0;
    CHECK(ctr_pak_entry(bapp.data, bapp.len, CTR_BANNER_NAME, &boff, &blen) && blen == CTR_BANNER_SIZE);
    CHECK(boff + blen <= bapp.len && !memcmp(bapp.data + boff, bnr.data, CTR_BANNER_SIZE));
    CHECK(ctr_pak_entry(bapp.data, bapp.len, "game.bin", &boff, &blen) && blen == 3 && !memcmp(bapp.data + boff, "abc", 3));
    CHECK(!ctr_pak_entry(bapp.data, bapp.len, "banner", &boff, &blen));
    CHECK(!ctr_pak_entry(app.data, app.len, CTR_BANNER_NAME, &boff, &blen));
    CHECK(ctr_pak_version(bapp.data, bapp.len) == PACK_VERSION && ctr_pak_version(app.data, app.len) != PACK_VERSION && ctr_pak_version(engine, code + smdh) == -1);
    free(wide);
    pack_free(&bp);
    buf_free(&bnr);
    buf_free(&bpak);
    buf_free(&bapp);
    free(engine);
    buf_free(&pak);
    buf_free(&icon);
    buf_free(&app);
    buf_free(&icon2);
    buf_free(&pak2);
    buf_free(&again);
    buf_free(&enc);
}

static void test_ivfc(void) {
    uint8_t* l3 = xmalloc(5000);
    for (int i = 0; i < 5000; i++) l3[i] = (uint8_t)(i * 31);
    Buf r = {0};
    ivfc_build(l3, 5000, &r);
    CHECK(!memcmp(r.data, "IVFC", 4) && rd32(r.data + 4) == 0x10000 && rd32(r.data + 8) == 32);
    CHECK(rd32(r.data + 0x14) == 32 && rd32(r.data + 0x24) == 0x1000 && rd32(r.data + 0x2C) == 64 && rd32(r.data + 0x3C) == 0x2000);
    CHECK(rd32(r.data + 0x44) == 5000 && rd32(r.data + 0x4C) == 12 && rd32(r.data + 0x54) == 0x5C);
    CHECK(r.len == 0x5000 && !memcmp(r.data + 0x1000, l3, 5000));
    uint8_t block[0x1000], h[32];
    memset(block, 0, sizeof(block));
    memcpy(block, l3 + 0x1000, 5000 - 0x1000);
    sha256(block, sizeof(block), h);
    CHECK(!memcmp(r.data + 0x4000 + 32, h, 32));
    sha256(r.data + 0x4000, 0x1000, h);
    CHECK(!memcmp(r.data + 0x3000, h, 32));
    sha256(r.data + 0x3000, 0x1000, h);
    CHECK(!memcmp(r.data + 0x60, h, 32));
    free(l3);
    buf_free(&r);
}

static void test_cwav(void) {
    const int16_t pcm[6] = {1, -1, 2, -2, 3, -3};
    Buf c = {0};
    cwav_build(pcm, 3, 2, 32000, &c);
    CHECK(c.len == 0x40 + 0x60 + 0x20 + 12 && !memcmp(c.data, "CWAV", 4) && rd16(c.data + 4) == 0xFEFF && rd32(c.data + 12) == c.len);
    CHECK(rd16(c.data + 0x14) == 0x7000 && rd32(c.data + 0x18) == 0x40 && rd32(c.data + 0x1C) == 0x60);
    CHECK(rd16(c.data + 0x20) == 0x7001 && rd32(c.data + 0x24) == 0xA0 && rd32(c.data + 0x28) == 0x2C);
    const uint8_t* in = c.data + 0x40;
    CHECK(!memcmp(in, "INFO", 4) && in[8] == 1 && rd32(in + 12) == 32000 && rd32(in + 20) == 3 && rd32(in + 28) == 2);
    CHECK(rd16(in + 32) == 0x7100 && rd32(in + 36) == 20 && rd32(in + 44) == 40);
    CHECK(rd16(in + 48) == 0x1F00 && rd32(in + 52) == 0x18 && rd32(in + 60) == 0xFFFFFFFFu && rd32(in + 72) == 0x18 + 6);
    const uint8_t* d = c.data + 0xA0;
    CHECK(!memcmp(d, "DATA", 4) && (int16_t)rd16(d + 0x20) == 1 && (int16_t)rd16(d + 0x24) == 3 && (int16_t)rd16(d + 0x26) == -1);
    buf_free(&c);
}

static void test_atrac3(void) {
    const size_t frames = 48000;
    float* pcm = xmalloc(frames * 2 * sizeof(float));
    for (size_t i = 0; i < frames; i++) pcm[i * 2] = pcm[i * 2 + 1] = 0.5f * (float)sin(i * 2 * 3.14159265358979 * 440 / 48000);
    size_t units = (frames + ATRAC3_DELAY + 1023) / 1024;
    Buf a = {0};
    CHECK(atrac3_encode(pcm, frames, 48000, -1, -1, &a));
    CHECK(a.len == 76 + units * 384 && !memcmp(a.data, "RIFF", 4) && rd32(a.data + 4) == a.len - 8 && !memcmp(a.data + 8, "WAVE", 4));
    CHECK(!memcmp(a.data + 12, "fmt ", 4) && rd32(a.data + 16) == 32 && rd16(a.data + 20) == 0x0270 && rd16(a.data + 22) == 2);
    CHECK(rd32(a.data + 24) == 48000 && rd16(a.data + 32) == 384 && !memcmp(a.data + 52, "fact", 4) && rd32(a.data + 60) == frames);
    CHECK(!memcmp(a.data + 68, "data", 4) && rd32(a.data + 72) == units * 384);
    size_t loud = 0;
    for (size_t i = 76; i < a.len; i++) loud += a.data[i] != 0;
    CHECK(loud > units * 100);
    Buf l = {0};
    CHECK(atrac3_encode(pcm, frames, 48000, 1000, 40000, &l));
    CHECK(!memcmp(l.data + 68, "smpl", 4) && rd32(l.data + 72) == 60 && rd32(l.data + 104) == 1);
    CHECK(rd32(l.data + 120) == 1000 + ATRAC3_SKIP && rd32(l.data + 124) == 40000 + ATRAC3_SKIP && !memcmp(l.data + 136, "data", 4));
    buf_free(&a);
    buf_free(&l);
    free(pcm);
}

static void test_iso_pack(void) {
    Pack pk = {0};
    buf_put(&pack_add(&pk, "maps/Map002.bin")->data, "bb", 2);
    buf_put(&pack_add(&pk, "game.bin")->data, "a", 1);
    Buf pak = {0};
    pack_build(&pk, &pak);
    const uint8_t* t = pak.data;
    CHECK(!memcmp(t, "TPAK", 4) && rd32(t + 4) == PACK_VERSION && rd32(t + 8) == 2);
    const char* names = (const char*)t + 16 + 2 * 12;
    CHECK(!strcmp(names + rd32(t + 16), "game.bin") && !strcmp(names + rd32(t + 28), "maps/Map002.bin"));
    CHECK(rd32(t + 20) % 64 == 0 && rd32(t + 24) == 1 && t[rd32(t + 20)] == 'a');
    CHECK(rd32(t + 32) % 64 == 0 && rd32(t + 36) == 2 && t[rd32(t + 32)] == 'b');
    uint8_t elf[64] = {0x7f, 'E', 'L', 'F', 1, 1, 1};
    elf[18] = 8;
    CHECK(elf_check(elf, sizeof(elf)));
    CHECK(!elf_check(pak.data, pak.len));
    Buf iso = {0};
    CHECK(ps2_iso_build(elf, sizeof(elf), pak.data, pak.len, &iso));
    CHECK(iso.len % ISO_SECTOR == 0 && !memcmp(iso.data + 16 * ISO_SECTOR + 1, "CD001", 5));
    size_t off = 0, size = 0;
    CHECK(iso_find(iso.data, iso.len, PS2_PAK_NAME, &off, &size) && size == pak.len && off % ISO_SECTOR == 0);
    CHECK(size == pak.len && !memcmp(iso.data + off, pak.data, pak.len));
    CHECK(iso_find(iso.data, iso.len, PS2_BOOT_NAME, &off, &size) && size == sizeof(elf) && !memcmp(iso.data + off, elf, sizeof(elf)));
    CHECK(iso_find(iso.data, iso.len, "SYSTEM.CNF", &off, &size));
    Buf cnf = {0};
    ps2_system_cnf(&cnf);
    CHECK(size == cnf.len && !memcmp(iso.data + off, cnf.data, cnf.len));
    CHECK(strstr((const char*)cnf.data, "cdrom0:\\" PS2_BOOT_NAME ";1") != NULL);
    CHECK(!iso_find(iso.data, iso.len, "MISSING.BIN", &off, &size));
    buf_free(&cnf);
    buf_free(&iso);
    buf_free(&pak);
    pack_free(&pk);
}

static void test_save_icon(void) {
    Buf sys = {0}, icn = {0};
    ps2_icon_sys("The Coffin of Andy and Leyley", &sys);
    CHECK(sys.len == 964 && !memcmp(sys.data, "PS2D", 4));
    CHECK(rd16(sys.data + 6) == 26 && sys.data[192] == 0x82 && sys.data[193] == 0x73);
    CHECK(!memcmp(sys.data + 260, "icon.icn", 9));
    Image* art = img_new(64, 64);
    ps2_icon_icn(art, &icn);
    CHECK(icn.len == 20 + 6 * 24 + 36 + 128 * 128 * 2 && rd32(icn.data) == 0x010000 && rd32(icn.data + 16) == 6);
    img_free(art);
    buf_free(&sys);
    buf_free(&icn);
}

static void test_pkg(void) {
    Buf sfo = {0};
    ps3_sfo_build("Test", PKG_TITLE_ID, &sfo);
    CHECK(sfo.len > 20 && !memcmp(sfo.data, "\0PSF", 4) && rd32(sfo.data + 16) == 12);
    const char* keys = (const char*)sfo.data + rd32(sfo.data + 8);
    CHECK(!strcmp(keys, "APP_VER"));
    Pack pk = {0};
    buf_put(&pack_add(&pk, "game.bin")->data, "abc", 3);
    Buf big = {0};
    for (int i = 0; i < 300001; i++) buf_u8(&big, (uint8_t)(i * 7 + (i >> 9)));
    PkgItem items[4] = {
        {"PARAM.SFO", 0, sfo.len, &sfo, NULL, NULL},
        {"USRDIR", 1, 0, NULL, NULL, NULL},
        {"USRDIR/DATA.BIN", 0, big.len, &big, NULL, NULL},
        {"USRDIR/TCOAAL.PAK", 0, pack_size(&pk), NULL, pack_stream, &pk},
    };
    const char* path = "tcoaal-test.pkg";
    uint64_t size = pkg_write(path, PKG_CONTENT_ID, items, 4, 4);
    CHECK(size > 0);
    Buf file = {0};
    CHECK(file_read(path, &file) && file.len == size);
    CHECK(file.len > 0x140 && rd32be(file.data) == 0x7F504B47 && rd32be(file.data + 20) == 4);
    CHECK(!memcmp(file.data + 48, PKG_CONTENT_ID, strlen(PKG_CONTENT_ID)));
    PkgFile* files = NULL;
    int count = 0;
    CHECK(pkg_read(file.data, file.len, &files, &count, 4) && count == 4);
    if (count == 4) {
        CHECK(!strcmp(files[0].name, "PARAM.SFO") && files[0].data.len == sfo.len && !memcmp(files[0].data.data, sfo.data, sfo.len));
        CHECK(!strcmp(files[1].name, "USRDIR") && files[1].dir);
        CHECK(!strcmp(files[2].name, "USRDIR/DATA.BIN") && files[2].data.len == big.len && !memcmp(files[2].data.data, big.data, big.len));
        Buf pak = {0};
        pack_build(&pk, &pak);
        CHECK(!strcmp(files[3].name, "USRDIR/TCOAAL.PAK") && files[3].data.len == pak.len && !memcmp(files[3].data.data, pak.data, pak.len));
        buf_free(&pak);
    }
    pkg_files_free(files, count);
    buf_free(&file);
    buf_free(&big);
    buf_free(&sfo);
    pack_free(&pk);
    remove(path);
}

static void test_pkg_reference(void) {
    Buf sfo = {0}, eboot = {0}, data = {0};
    ps3_sfo_build("Test", PKG_TITLE_ID, &sfo);
    for (int i = 0; i < 256; i++) buf_u8(&eboot, (uint8_t)(i * 13));
    for (int i = 0; i < 1000; i++) buf_u8(&data, (uint8_t)(i ^ 0x5A));
    PkgItem items[4] = {
        {"PARAM.SFO", 0, sfo.len, &sfo, NULL, NULL},
        {"USRDIR", 1, 0, NULL, NULL, NULL},
        {"USRDIR/DATA.BIN", 0, data.len, &data, NULL, NULL},
        {"USRDIR/EBOOT.BIN", 0, eboot.len, &eboot, NULL, NULL},
    };
    const char* path = "tcoaal-ref.pkg";
    CHECK(pkg_write(path, PKG_CONTENT_ID, items, 4, 2) == 2960);
    Buf file = {0};
    CHECK(file_read(path, &file));
    uint8_t hash[20];
    sha1(file.data, file.len, hash);
    char hex[41];
    for (int i = 0; i < 20; i++) snprintf(hex + i * 2, 3, "%02x", hash[i]);
    CHECK(!strcmp(hex, "69e477d97e1909b0ddef67311ef731851679782d"));
    buf_free(&file);
    buf_free(&sfo);
    buf_free(&eboot);
    buf_free(&data);
    remove(path);
}

static const uint8_t* disc_entry(const Buf* pak, const char* name, uint32_t* size) {
    uint32_t count = rd32(pak->data + 8), h = pack_hash(name);
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t* e = pak->data + 16 + i * 12;
        if (rd32(e) != h) continue;
        *size = rd32(e + 8);
        return pak->data + (size_t)rd32(e + 4) * PACK_SECTOR;
    }
    return NULL;
}

static void test_disc_pack(void) {
    CHECK(pack_hash("") == 2166136261u && pack_hash("a") == 0xE40C292Cu);
    Pack pk = {0};
    buf_zero(&pack_add(&pk, "big.bin")->data, 5000);
    buf_put(&pack_add(&pk, "title.tex")->data, "tex", 3);
    Buf pak = {0};
    pack_build_disc(&pk, &pak);
    CHECK(!memcmp(pak.data, "TPAK", 4) && rd32(pak.data + 4) == PACK_DISC_VERSION && rd32(pak.data + 8) == 2);
    CHECK(pak.len % PACK_SECTOR == 0 && rd32(pak.data + 12) == pak.len / PACK_SECTOR);
    CHECK(rd32(pak.data + 16) < rd32(pak.data + 28));
    uint32_t size = 0;
    const uint8_t* tex = disc_entry(&pak, "title.tex", &size);
    CHECK(tex && size == 3 && !memcmp(tex, "tex", 3));
    const uint8_t* big = disc_entry(&pak, "big.bin", &size);
    CHECK(big && size == 5000 && (size_t)(big - pak.data) % PACK_SECTOR == 0);
    pack_free(&pk);
    buf_free(&pak);
}

static void fake_exe(Buf* b, size_t text) {
    buf_zero(b, DISC_DATA + text);
    memcpy(b->data, "PS-X EXE", 8);
    b->data[0x1C] = (uint8_t)text;
    b->data[0x1D] = (uint8_t)(text >> 8);
    b->data[0x1E] = (uint8_t)(text >> 16);
    memset(b->data + DISC_DATA, 0xAB, text);
}

static int sector_ok(const uint8_t* s) { return disc_edc(s + 16, 8 + DISC_DATA) == rd32(s + 2072); }

static void test_disc(void) {
    Buf exe = {0}, pak = {0}, bin = {0}, cue = {0};
    fake_exe(&exe, DISC_DATA);
    buf_zero(&pak, 3 * DISC_DATA);
    pak.data[0] = 'P';
    CHECK(exe_check(exe.data, exe.len));
    CHECK(!exe_check(exe.data, exe.len - 1));
    CHECK(ps1_disc_build(exe.data, exe.len, pak.data, pak.len, &bin));
    CHECK(bin.len % DISC_RAW == 0);
    const uint8_t* pvd = bin.data + 16 * DISC_RAW;
    CHECK(pvd[0] == 0 && pvd[1] == 0xFF && pvd[11] == 0 && pvd[12] == 0x00 && pvd[13] == 0x02 && pvd[14] == 0x16 && pvd[15] == 2);
    CHECK(!memcmp(pvd + 24 + 1, "CD001", 5) && !memcmp(pvd + 24 + 8, "PLAYSTATION", 11) && !memcmp(pvd + 24 + 1024, "CD-XA001", 8));
    int sealed = 1;
    for (size_t s = 0; s < bin.len / DISC_RAW; s++) sealed &= sector_ok(bin.data + s * DISC_RAW);
    CHECK(sealed);
    uint32_t lba = 0, size = 0, room = 0;
    CHECK(disc_find(bin.data, bin.len, DISC_EXE, &lba, &size, &room));
    CHECK(size == exe.len && room == DISC_EXE_SECTORS);
    CHECK(!memcmp(bin.data + (size_t)lba * DISC_RAW + 24, "PS-X EXE", 8));
    CHECK(bin.data[(size_t)(lba + 1) * DISC_RAW + 18] == 0x89);
    CHECK(disc_find(bin.data, bin.len, DISC_PAK, &lba, &size, &room));
    CHECK(size == pak.len && bin.data[(size_t)lba * DISC_RAW + 24] == 'P');
    Buf bigger = {0};
    fake_exe(&bigger, 5 * DISC_DATA);
    CHECK(disc_replace(bin.data, bin.len, DISC_EXE, bigger.data, bigger.len));
    CHECK(disc_find(bin.data, bin.len, DISC_EXE, &lba, &size, &room) && size == bigger.len);
    CHECK(bin.data[(size_t)(lba + 5) * DISC_RAW + 24] == 0xAB && bin.data[(size_t)(lba + 5) * DISC_RAW + 18] == 0x89);
    sealed = 1;
    for (size_t s = 0; s < bin.len / DISC_RAW; s++) sealed &= sector_ok(bin.data + s * DISC_RAW);
    CHECK(sealed);
    disc_cue("TCOAAL.bin", &cue);
    buf_u8(&cue, 0);
    CHECK(strstr((const char*)cue.data, "FILE \"TCOAAL.bin\" BINARY") && strstr((const char*)cue.data, "MODE2/2352"));
    buf_free(&exe);
    buf_free(&bigger);
    buf_free(&pak);
    buf_free(&bin);
    buf_free(&cue);
}

static void test_psx_texture(void) {
    Image* img = img_new(5, 4);
    for (int i = 0; i < 20; i++) {
        uint8_t* p = img->px + i * 4;
        p[0] = (uint8_t)(i * 12);
        p[1] = 40;
        p[2] = 200;
        p[3] = (i % 5 == 0 || i < 5) ? 0 : 255;
    }
    Buf t = {0};
    CHECK(psx_tex_encode(img, 20, 16, PSX_4BIT, &t));
    CHECK(!memcmp(t.data, "PTEX", 4) && t.data[4] == PSX_4BIT && t.data[5] == 0);
    CHECK(rd16(t.data + 6) == 1 && rd16(t.data + 8) == 4 && rd16(t.data + 10) == 3);
    CHECK(rd16(t.data + 12) == 1 && rd16(t.data + 14) == 1 && rd16(t.data + 16) == 5 && rd16(t.data + 18) == 4);
    CHECK(rd16(t.data + 20) == 20 && rd16(t.data + 22) == 16);
    int stp = 1;
    for (int i = 0; i < 16; i++) stp &= rd16(t.data + 24 + i * 2) == 0 || (rd16(t.data + 24 + i * 2) & 0x8000);
    CHECK(stp);
    size_t table = 24 + 32;
    CHECK(rd16(t.data + table + 4) == 4 && rd16(t.data + table + 6) == 3);
    CHECK(t.len == table + 8 + 3 * 2 + 2);
    buf_free(&t);
    img->px[(1 * 5 + 2) * 4 + 3] = 0;
    CHECK(psx_tex_encode(img, 20, 16, PSX_8BIT, &t));
    CHECK(t.data[5] == 1 && rd16(t.data + 24) == 0);
    buf_free(&t);
    target_set(target_get(TARGET_PS1));
    CHECK(tex_from_image(img, 0, 0, &t) && !memcmp(t.data, "PTEX", 4) && t.data[4] == PSX_8BIT);
    buf_free(&t);
    target_set(target_get(TARGET_PSP));
    img_free(img);
    Image* wide = img_new(300, 10);
    for (int i = 0; i < 3000; i++) {
        wide->px[i * 4] = (uint8_t)(i % 300);
        wide->px[i * 4 + 3] = 255;
    }
    CHECK(psx_tex_encode(wide, 1200, 40, PSX_8BIT, &t));
    table = 24 + 512;
    CHECK(rd16(t.data + 6) == 2 && rd16(t.data + table + 4) == 150 && rd16(t.data + table + 8) == 150 && rd16(t.data + table + 12) == 150);
    CHECK(t.len == table + 16 + 2 * (75 * 10) * 2);
    buf_free(&t);
    img_free(wide);
    CHECK(psx_scaled(1280) == 320 && psx_scaled(1919) == 480 && psx_scaled(1) == 1);
}

int main(void) {
    test_crypto();
    test_json_vbin();
    test_json_vbn2();
    test_scripts();
    test_texture_alpha_flags();
    test_texture_psp();
    test_texture_3ds();
    test_texture_ps2();
    test_texture_ps3();
    test_texture_linear();
    test_texture_wii();
    target_set(target_get(TARGET_PSP));
    test_png_indexed();
    test_vpk();
    test_wuhb();
    test_pbp_pack();
    test_pack();
    test_3dsx();
    test_ivfc();
    test_cwav();
    test_atrac3();
    test_iso_pack();
    test_save_icon();
    test_pkg();
    test_pkg_reference();
    test_disc_pack();
    test_disc();
    test_psx_texture();
    printf("%d checks, %d failed\n", g_run, g_fail);
    return g_fail ? 1 : 0;
}
