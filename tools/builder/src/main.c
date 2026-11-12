#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "adpcm.h"
#include "cia.h"
#include "crypto.h"
#include "cwav.h"
#include "detect.h"
#include "disc.h"
#include "events.h"
#include "font.h"
#include "game.h"
#include "image.h"
#include "iso.h"
#include "jsmini.h"
#include "keys.h"
#include "node.h"
#include "pack.h"
#include "pkg.h"
#include "snd0.h"
#include "ps2icon.h"
#include "psxtex.h"
#include "target.h"
#include "ui.h"
#include "util.h"
#include "vpk.h"
#include "wuhb.h"

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#endif

#define BLOB(sym)                         \
    extern const unsigned char sym[];     \
    extern const size_t sym##_size;
BLOB(embedded_psp_eboot)
BLOB(embedded_vita_eboot)
BLOB(embedded_vita_sfo)
BLOB(embedded_3ds_engine)
BLOB(embedded_3ds_cia)
BLOB(embedded_ps2_elf)
BLOB(embedded_wii_dol)
BLOB(embedded_wiiu_rpx)
BLOB(embedded_ps3_eboot)
BLOB(embedded_xenon_elf)
BLOB(embedded_ps1_exe)
BLOB(embedded_psp_art)
BLOB(embedded_vita_art)
BLOB(embedded_3ds_art)
BLOB(embedded_wii_art)
BLOB(embedded_wiiu_art)
BLOB(embedded_ps2_art)
BLOB(embedded_ps3_art)
BLOB(embedded_xenon_art)

typedef struct {
    const uint8_t* data;
    size_t size;
} Blob;

static Blob engine_blob(TargetId id) {
    switch (id) {
        case TARGET_VITA:
            return (Blob){embedded_vita_eboot, embedded_vita_eboot_size};
        case TARGET_3DS:
            return (Blob){embedded_3ds_engine, embedded_3ds_engine_size};
        case TARGET_WII:
            return (Blob){embedded_wii_dol, embedded_wii_dol_size};
        case TARGET_WIIU:
            return (Blob){embedded_wiiu_rpx, embedded_wiiu_rpx_size};
        case TARGET_PS2:
            return (Blob){embedded_ps2_elf, embedded_ps2_elf_size};
        case TARGET_PS3:
            return (Blob){embedded_ps3_eboot, embedded_ps3_eboot_size};
        case TARGET_XENON:
            return (Blob){embedded_xenon_elf, embedded_xenon_elf_size};
        case TARGET_PS1:
            return (Blob){embedded_ps1_exe, embedded_ps1_exe_size};
        default:
            return (Blob){embedded_psp_eboot, embedded_psp_eboot_size};
    }
}

static Blob art_blob(TargetId id) {
    switch (id) {
        case TARGET_VITA:
            return (Blob){embedded_vita_art, embedded_vita_art_size};
        case TARGET_3DS:
            return (Blob){embedded_3ds_art, embedded_3ds_art_size};
        case TARGET_WII:
            return (Blob){embedded_wii_art, embedded_wii_art_size};
        case TARGET_WIIU:
            return (Blob){embedded_wiiu_art, embedded_wiiu_art_size};
        case TARGET_XENON:
            return (Blob){embedded_xenon_art, embedded_xenon_art_size};
        case TARGET_PS2:
        case TARGET_PS1:
            return (Blob){embedded_ps2_art, embedded_ps2_art_size};
        case TARGET_PS3:
            return (Blob){embedded_ps3_art, embedded_ps3_art_size};
        default:
            return (Blob){embedded_psp_art, embedded_psp_art_size};
    }
}

static const char* SUPPORTED[] = {"3.0.12", NULL};
static const double BANNER_SECONDS = 2.9;
static const int FONT_SIZES[] = {18, 20, 22, 24, 26, 28, 30, 32, 36, 40, 42};
static const char* SYSTEM_NAMES[] = {"Balloon", "IconSet", "Window", "msgimg_0", "msgimg_1", "msgimg_2", "VNButtons", "Shadow1", "Shadow2",
                                     "ButtonSet", "GameOver", "Loading", "Damage", "States", "Weapons1", "Weapons2", "Weapons3", NULL};
static const char* TITLE = "The Coffin of Andy and Leyley";

typedef struct {
    char** items;
    int count;
    int cap;
} StrList;

static void sl_add(StrList* l, char* s) {
    if (l->count == l->cap) {
        l->cap = l->cap ? l->cap * 2 : 64;
        l->items = xrealloc(l->items, sizeof(char*) * (size_t)l->cap);
    }
    l->items[l->count++] = s;
}

static void sl_free(StrList* l) {
    for (int i = 0; i < l->count; i++) free(l->items[i]);
    free(l->items);
    memset(l, 0, sizeof(*l));
}

static StrList g_errors;

static void problem(const char* fmt, const char* where, int idx, double code, const char* msg) {
    sl_add(&g_errors, str_fmt(fmt, where, idx, (int)code, msg));
}

static int is_code(const Node* cmd, int code) { return node_numv(node_get(cmd, "code"), -1) == code; }

static Node* params_of(Node* cmd) {
    Node* p = node_get(cmd, "parameters");
    return p && p->type == N_ARR ? p : NULL;
}

static void compile_route_steps(Node* route_list, const char* where, int idx, double code) {
    if (!route_list || route_list->type != N_ARR) return;
    for (size_t s = 0; s < route_list->count; s++) {
        Node* step = route_list->items[s];
        if (!is_code(step, 45)) continue;
        Node* sp = params_of(step);
        const char* src = sp && sp->count ? node_strv(sp->items[0]) : "";
        if (!src) src = "";
        char err[160];
        Node* compiled = js_compile_script(src, err, sizeof(err));
        if (!compiled) {
            problem("%s[%d] code %d: %s", where, idx, code, err);
            return;
        }
        Node* np = node_arr();
        node_push(np, sp && sp->count ? node_clone(sp->items[0]) : node_str(""));
        node_push(np, compiled);
        node_set(step, "parameters", np);
    }
}

static void compile_list(Node* lst, const char* where) {
    if (!lst || lst->type != N_ARR) return;
    for (size_t i = 0; i < lst->count; i++) {
        Node* cmd = lst->items[i];
        double code = node_numv(node_get(cmd, "code"), -1);
        Node* p = params_of(cmd);
        char err[160];
        if (code == 355) {
            Buf body = {0};
            const char* first = p && p->count ? node_strv(p->items[0]) : NULL;
            if (!first) continue;
            buf_put(&body, first, strlen(first));
            for (size_t j = i + 1; j < lst->count && is_code(lst->items[j], 655); j++) {
                Node* pj = params_of(lst->items[j]);
                const char* s = pj && pj->count ? node_strv(pj->items[0]) : "";
                buf_u8(&body, '\n');
                if (s) buf_put(&body, s, strlen(s));
            }
            buf_u8(&body, 0);
            Node* compiled = js_compile_script((const char*)body.data, err, sizeof(err));
            buf_free(&body);
            if (!compiled) {
                problem("%s[%d] code %d: %s", where, (int)i, code, err);
                continue;
            }
            Node* np = node_arr();
            node_push(np, node_clone(p->items[0]));
            node_push(np, compiled);
            node_set(cmd, "parameters", np);
        } else if (code == 111 && p && p->count > 1 && node_numv(p->items[0], -1) == 12) {
            const char* src = node_strv(p->items[1]);
            Node* compiled = src ? js_compile_expr(src, err, sizeof(err)) : NULL;
            if (!compiled) {
                problem("%s[%d] code %d: %s", where, (int)i, code, src ? err : "not a string");
                continue;
            }
            Node* np = node_arr();
            node_push(np, node_num(12));
            node_push(np, node_clone(p->items[1]));
            node_push(np, compiled);
            node_set(cmd, "parameters", np);
        } else if (code == 122 && p && p->count > 4 && node_numv(p->items[3], -1) == 4) {
            const char* src = node_strv(p->items[4]);
            Node* compiled = src ? js_compile_expr(src, err, sizeof(err)) : NULL;
            if (!compiled) {
                problem("%s[%d] code %d: %s", where, (int)i, code, src ? err : "not a string");
                continue;
            }
            Node* np = node_arr();
            for (int k = 0; k < 5; k++) node_push(np, node_clone(p->items[k]));
            node_push(np, compiled);
            node_set(cmd, "parameters", np);
        } else if (code == 205 && p && p->count > 1) {
            compile_route_steps(node_get(p->items[1], "list"), where, (int)i, code);
        }
    }
}

static Node* load_lang(Game* g) { return game_language(g); }

static void add_se(Game* g, const char* name, Node* out) {
    if (!name || !*name) return;
    for (size_t i = 0; i < out->count; i++) {
        if (!strcmp(out->items[i]->str, name)) return;
    }
    char* rel = str_fmt("audio/se/%s.ogg", name);
    char* stored = game_resolve(g, rel);
    if (stored) node_push(out, node_str(name));
    free(stored);
    free(rel);
}

static void add_sound_refs(Game* g, const Node* n, Node* out) {
    if (!n || (n->type != N_ARR && n->type != N_OBJ)) return;
    if (n->type == N_OBJ) {
        double code = node_numv(node_get(n, "code"), -1);
        Node* sound = node_at(node_get(n, "parameters"), 0);
        if ((code == 250 || code == 44) && sound && sound->type == N_OBJ) add_se(g, node_strv(node_get(sound, "name")), out);
    }
    for (size_t i = 0; i < n->count; i++) add_sound_refs(g, n->items[i], out);
}

static void add_line_refs(const Node* n, const Node* all, Node* out) {
    if (!n) return;
    if (n->type == N_STR) {
        for (const char* p = strstr(n->str, "(lines)["); p; p = strstr(p + 1, "(lines)[")) {
            const char* id = p + 8;
            const char* end = strchr(id, ']');
            if (!end || end == id) continue;
            char* key = xstrndup(id, (size_t)(end - id));
            Node* v = node_get(out, key) ? NULL : node_get(all, key);
            if (v) node_set(out, key, node_clone(v));
            free(key);
        }
    } else if (n->type == N_ARR || n->type == N_OBJ) {
        for (size_t i = 0; i < n->count; i++) add_line_refs(n->items[i], all, out);
    }
}

static char* credits_text(Game* g, const char* voices) {
    Buf b = {0};
    if (!game_read(g, "data/Credits.txt", &b)) return xstrdup("");
    Buf out = {0};
    size_t i = 0;
    while (i < b.len) {
        uint8_t c = b.data[i];
        int n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
        int ok = n > 0 && i + (size_t)n <= b.len;
        for (int k = 1; ok && k < n; k++) ok = (b.data[i + (size_t)k] & 0xC0) == 0x80;
        if (ok) {
            buf_put(&out, b.data + i, (size_t)n);
            i += (size_t)n;
        } else {
            buf_put(&out, "\xEF\xBF\xBD", 3);
            i++;
        }
    }
    buf_u8(&out, 0);
    buf_free(&b);
    char* text = (char*)out.data;
    const char* head = "This game was created with RPG Maker MV";
    const char* tail = "(EULA).";
    char* repl = str_fmt("The original game was created with RPG Maker MV by KADOKAWA CORPORATION.\nFor this %s port, the engine was rewritten from scratch.", g_target->console);
    Buf res = {0};
    const char* s = text;
    for (;;) {
        const char* a = strstr(s, head);
        const char* e = a ? strstr(a + strlen(head), tail) : NULL;
        if (!a || !e) {
            buf_put(&res, s, strlen(s));
            break;
        }
        buf_put(&res, s, (size_t)(a - s));
        buf_put(&res, repl, strlen(repl));
        s = e + strlen(tail);
    }
    if (voices) {
        char* vc = path_join(voices, "credits.txt");
        Buf extra = {0};
        if (file_read(vc, &extra) && extra.len) {
            const char* block = "\r\n<block:-1,0,10,10,100,center,black>\r\n";
            buf_put(&res, block, strlen(block));
            for (size_t i = 0; i < extra.len; i++) {
                if (extra.data[i] == '\r') continue;
                if (extra.data[i] == '\n') buf_put(&res, "\r\n", 2);
                else buf_u8(&res, extra.data[i]);
            }
            const char* end = "\r\n</block>\r\n";
            buf_put(&res, end, strlen(end));
        }
        buf_free(&extra);
        free(vc);
    }
    buf_u8(&res, 0);
    free(repl);
    free(text);
    return (char*)res.data;
}

static Node* text_colors(Game* g) {
    Node* arr = node_arr();
    Buf b = {0};
    if (!game_read(g, "img/system/Window.png", &b)) return arr;
    Image* img = img_load_png(b.data, b.len);
    buf_free(&b);
    if (!img) return arr;
    for (int n = 0; n < 32; n++) {
        int x = 96 + (n % 8) * 12 + 6, y = 144 + (n / 8) * 12 + 6;
        Node* c = node_arr();
        for (int k = 0; k < 3; k++) node_push(c, node_num(x < img->w && y < img->h ? img->px[(y * img->w + x) * 4 + k] : 0));
        node_push(arr, c);
    }
    img_free(img);
    return arr;
}

static Node* build_database(Game* g, Pack* pack, const char* voices, Node** lang_out, char** credits_out) {
    Node* system = game_json(g, "data/System.json");
    const char* drop[] = {"switches", "variables", "armorTypes", "weaponTypes", "skillTypes", "elements", "equipTypes", "testBattlers", NULL};
    for (int i = 0; drop[i]; i++) node_remove(system, drop[i]);
    Node* actors = game_json(g, "data/Actors.json");
    Node* items = game_json(g, "data/Items.json");
    Node* common = game_json(g, "data/CommonEvents.json");
    for (size_t i = 0; i < common->count; i++) {
        Node* ce = common->items[i];
        if (ce->type != N_OBJ) continue;
        char* where = str_fmt("ce%d", (int)node_numv(node_get(ce, "id"), 0));
        compile_list(node_get(ce, "list"), where);
        free(where);
    }
    Node* infos = game_json(g, "data/MapInfos.json");
    Node* tilesets = node_obj();
    Node* ts = game_json(g, "data/Tilesets.json");
    for (size_t i = 0; i < ts->count; i++) {
        Node* t = ts->items[i];
        if (t->type != N_OBJ) continue;
        char* key = str_fmt("%d", (int)node_numv(node_get(t, "id"), 0));
        Node* flags = node_take(t, "flags");
        node_set(tilesets, key, flags ? flags : node_arr());
        free(key);
    }
    node_free(ts);
    Node* plugins = node_obj();
    Node* pl = game_plugins(g);
    for (size_t i = 0; i < pl->count; i++) {
        Node* p = pl->items[i];
        const char* name = node_strv(node_get(p, "name"));
        Node* params = node_take(p, "parameters");
        if (name) node_set(plugins, name, params ? params : node_obj());
        else node_free(params);
    }
    node_free(pl);
    Node* lang = load_lang(g);
    Node* lang_out_node = node_obj();
    const char* lk[][2] = {{"lines", "linesLUT"}, {"labels", "labelLUT"}, {"menus", "sysMenus"}, {"sys", "sysLabel"}, {"info", "langInfo"}, {"fontSize", "fontSize"}};
    for (int i = 0; i < 6; i++) {
        Node* v = node_get(lang, lk[i][1]);
        node_set(lang_out_node, lk[i][0], v ? node_clone(v) : node_null());
    }
    node_free(lang);
    int ps1 = g_target->id == TARGET_PS1;
    if (ps1) node_sort_keys(node_get(lang_out_node, "labels"));
    *lang_out = node_clone(lang_out_node);
    if (ps1) {
        Node* common_lines = node_obj();
        add_line_refs(common, node_get(*lang_out, "lines"), common_lines);
        add_line_refs(items, node_get(*lang_out, "lines"), common_lines);
        node_sort_keys(common_lines);
        node_set(lang_out_node, "lines", common_lines);
    }
    char* credits = credits_text(g, voices);
    Node* root = node_obj();
    node_set(root, "version", node_str(g->version));
    node_set(root, "system", system);
    node_set(root, "actors", actors);
    node_set(root, "items", items);
    node_set(root, "commonEvents", common);
    node_set(root, "mapInfos", infos);
    node_set(root, "tilesets", tilesets);
    node_set(root, "plugins", plugins);
    node_set(root, "lang", lang_out_node);
    node_set(root, "credits", node_str(credits));
    node_set(root, "textColors", text_colors(g));
    if (ps1) {
        Node* ses = node_arr();
        Node* sounds = node_get(system, "sounds");
        for (size_t i = 0; sounds && i < sounds->count; i++) add_se(g, node_strv(node_get(sounds->items[i], "name")), ses);
        add_sound_refs(g, common, ses);
        node_set(root, "ses", ses);
        events_compact_common(common);
    }
    PackEntry* e = pack_add(pack, "game.bin");
    vbin_dump(root, &e->data);
    *credits_out = credits;
    return root;
}

static uint8_t tile_at(const Node* data, int w, int h, int x, int y, int z, double* out) {
    size_t idx = ((size_t)z * (size_t)h + (size_t)y) * (size_t)w + (size_t)x;
    const Node* v = node_at(data, idx);
    double d = v && v->type == N_NUM ? v->num : 0;
    *out = d;
    return 0;
}

static int flag_of(const Node* flags, double t) {
    if (t < 0) return 0;
    const Node* f = node_at(flags, (size_t)t);
    return f && f->type == N_NUM ? (int)f->num : 0;
}

static void passability(const Node* m, const Node* flags, Buf* pas, Buf* region, Buf* tflags) {
    int w = (int)node_numv(node_get(m, "width"), 0), h = (int)node_numv(node_get(m, "height"), 0);
    const Node* data = node_get(m, "data");
    buf_zero(pas, (size_t)w * (size_t)h);
    buf_zero(region, (size_t)w * (size_t)h);
    buf_zero(tflags, (size_t)w * (size_t)h);
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            double tiles[4];
            for (int i = 0; i < 4; i++) tile_at(data, w, h, x, y, 3 - i, &tiles[i]);
            int v = 0;
            for (int bitn = 0; bitn < 4; bitn++) {
                int bit = 1 << bitn;
                int ok = 0;
                for (int i = 0; i < 4; i++) {
                    int f = flag_of(flags, tiles[i]);
                    if (f & 0x10) continue;
                    if ((f & bit) == 0) {
                        ok = 1;
                        break;
                    }
                    if ((f & bit) == bit) {
                        ok = 0;
                        break;
                    }
                }
                if (ok) v |= 1 << bitn;
            }
            pas->data[y * w + x] = (uint8_t)v;
            double r;
            tile_at(data, w, h, x, y, 5, &r);
            region->data[y * w + x] = (uint8_t)((int64_t)r & 0xFF);
            int fl = 0;
            for (int i = 0; i < 4; i++) {
                int f = flag_of(flags, tiles[i]);
                if (f & 0x20) fl |= 1;
                if (f & 0x40) fl |= 2;
                if (f & 0x80) fl |= 4;
            }
            tflags->data[y * w + x] = (uint8_t)fl;
        }
    }
}

typedef struct {
    Game* game;
    Node* tilesets;
    int* ids;
    PackEntry** entries;
    StrList* errors;
} MapJobs;

static int build_maps(Game* g, Node* tilesets, const Node* lines, Pack* pack) {
    Node* infos = game_json(g, "data/MapInfos.json");
    int count = 0;
    for (size_t i = 0; i < infos->count; i++) {
        Node* info = infos->items[i];
        if (info->type != N_OBJ) continue;
        int id = (int)node_numv(node_get(info, "id"), 0);
        char* path = str_fmt("data/Map%03d.json", id);
        char* stored = game_resolve(g, path);
        if (!stored) {
            free(path);
            continue;
        }
        free(stored);
        Node* m = game_json(g, path);
        free(path);
        char* tkey = str_fmt("%d", (int)node_numv(node_get(m, "tilesetId"), 0));
        Node* flags = node_get(tilesets, tkey);
        free(tkey);
        Node* empty = node_arr();
        Buf pas = {0}, region = {0}, tfl = {0};
        passability(m, flags ? flags : empty, &pas, &region, &tfl);
        node_free(empty);
        Node* events = node_get(m, "events");
        for (size_t e = 0; events && e < events->count; e++) {
            Node* ev = events->items[e];
            if (ev->type != N_OBJ) continue;
            int evid = (int)node_numv(node_get(ev, "id"), 0);
            Node* pages = node_get(ev, "pages");
            for (size_t pi = 0; pages && pi < pages->count; pi++) {
                Node* page = pages->items[pi];
                char* where = str_fmt("map%d/ev%d/p%d", id, evid, (int)pi);
                compile_list(node_get(page, "list"), where);
                free(where);
                Node* route = node_get(page, "moveRoute");
                Node* rl = route ? node_get(route, "list") : NULL;
                for (size_t s = 0; rl && rl->type == N_ARR && s < rl->count; s++) {
                    Node* step = rl->items[s];
                    if (!is_code(step, 45)) continue;
                    Node* sp = params_of(step);
                    const char* src = sp && sp->count ? node_strv(sp->items[0]) : "";
                    if (!src) src = "";
                    char err[160];
                    Node* compiled = js_compile_script(src, err, sizeof(err));
                    if (!compiled) {
                        sl_add(&g_errors, str_fmt("route map%d/ev%d: %s", id, evid, err));
                        continue;
                    }
                    Node* np = node_arr();
                    node_push(np, sp && sp->count ? node_clone(sp->items[0]) : node_str(""));
                    node_push(np, compiled);
                    node_set(step, "parameters", np);
                }
            }
        }
        node_remove(m, "data");
        node_remove(m, "encounters");
        if (g_target->id == TARGET_PS1) {
            Node* map_lines = node_obj();
            add_line_refs(m, lines, map_lines);
            node_sort_keys(map_lines);
            node_set(m, "lines", map_lines);
            Node* ses = node_arr();
            add_sound_refs(g, node_get(m, "events"), ses);
            node_set(m, "ses", ses);
            events_compact_map(m);
        }
        node_set(m, "id", node_num(id));
        node_set(m, "pass", node_bytes(pas.data, pas.len));
        node_set(m, "region", node_bytes(region.data, region.len));
        node_set(m, "flags", node_bytes(tfl.data, tfl.len));
        buf_free(&pas);
        buf_free(&region);
        buf_free(&tfl);
        char* name = str_fmt("maps/Map%03d.bin", id);
        PackEntry* pe = pack_add(pack, name);
        free(name);
        vbin_dump(m, &pe->data);
        node_free(m);
        count++;
    }
    node_free(infos);
    return count;
}

typedef struct {
    char** stored;
    char** names;
    int count;
    int cap;
} Registry;

static void reg_add(Game* g, Registry* r, const char* folder, const char* name) {
    if (!name || !*name) return;
    char* path = str_fmt("%s/%s.png", folder, name);
    char* stored = game_resolve(g, path);
    free(path);
    if (!stored) return;
    for (int i = 0; i < r->count; i++) {
        if (!strcmp(r->stored[i], stored)) {
            free(r->names[i]);
            r->names[i] = xstrdup(name);
            free(stored);
            return;
        }
    }
    if (r->count == r->cap) {
        r->cap = r->cap ? r->cap * 2 : 128;
        r->stored = xrealloc(r->stored, sizeof(char*) * (size_t)r->cap);
        r->names = xrealloc(r->names, sizeof(char*) * (size_t)r->cap);
    }
    r->stored[r->count] = stored;
    r->names[r->count] = xstrdup(name);
    r->count++;
}

static const char* reg_get(const Registry* r, const char* stored) {
    for (int i = 0; i < r->count; i++) {
        if (!strcmp(r->stored[i], stored)) return r->names[i];
    }
    return NULL;
}

static void build_registry(Game* g, Node* actors, Registry* r) {
    for (size_t i = 0; actors && i < actors->count; i++) {
        Node* a = actors->items[i];
        if (a->type != N_OBJ) continue;
        reg_add(g, r, "img/characters", node_strv(node_get(a, "characterName")));
        reg_add(g, r, "img/faces", node_strv(node_get(a, "faceName")));
    }
    for (int i = 0; SYSTEM_NAMES[i]; i++) reg_add(g, r, "img/system", SYSTEM_NAMES[i]);
    for (int i = 0; i < 32; i++) {
        char* n = str_fmt("msgimg_%d", i);
        reg_add(g, r, "img/system", n);
        free(n);
    }
    Node* infos = game_json(g, "data/MapInfos.json");
    for (size_t i = 0; i < infos->count; i++) {
        Node* info = infos->items[i];
        if (info->type != N_OBJ) continue;
        char* path = str_fmt("data/Map%03d.json", (int)node_numv(node_get(info, "id"), 0));
        char* stored = game_resolve(g, path);
        if (!stored) {
            free(path);
            continue;
        }
        free(stored);
        Node* m = game_json(g, path);
        free(path);
        Node* events = node_get(m, "events");
        for (size_t e = 0; events && e < events->count; e++) {
            Node* ev = events->items[e];
            if (ev->type != N_OBJ) continue;
            Node* pages = node_get(ev, "pages");
            for (size_t pi = 0; pages && pi < pages->count; pi++) {
                Node* page = pages->items[pi];
                reg_add(g, r, "img/characters", node_strv(node_get(node_get(page, "image"), "characterName")));
                Node* list = node_get(page, "list");
                for (size_t c = 0; list && c < list->count; c++) {
                    Node* cmd = list->items[c];
                    if (!is_code(cmd, 205)) continue;
                    Node* p = params_of(cmd);
                    Node* rl = p && p->count > 1 ? node_get(p->items[1], "list") : NULL;
                    for (size_t s = 0; rl && s < rl->count; s++) {
                        Node* st = rl->items[s];
                        if (!is_code(st, 41)) continue;
                        Node* sp = params_of(st);
                        reg_add(g, r, "img/characters", sp && sp->count ? node_strv(sp->items[0]) : NULL);
                    }
                }
            }
        }
        node_free(m);
    }
    node_free(infos);
}

typedef struct {
    Game* game;
    char* stored;
    const char* name;
    PackEntry* entry;
} ImageJob;

static int png_size(const Buf* b, int* w, int* h) {
    if (b->len < 24 || memcmp(b->data, "\x89PNG", 4) != 0) return 0;
    *w = (int)((uint32_t)b->data[16] << 24 | (uint32_t)b->data[17] << 16 | (uint32_t)b->data[18] << 8 | b->data[19]);
    *h = (int)((uint32_t)b->data[20] << 24 | (uint32_t)b->data[21] << 16 | (uint32_t)b->data[22] << 8 | b->data[23]);
    return 1;
}

static void image_job(void* ctx, int index) {
    ImageJob* job = &((ImageJob*)ctx)[index];
    Buf b = {0};
    if (!game_read_stored(job->game, job->stored, &b)) {
        job->entry->error = str_fmt("%s: unreadable", job->stored);
        return;
    }
    const char* slash = strrchr(job->stored, '/');
    size_t flen = (size_t)(slash - job->stored);
    int cx = 0, cy = 0;
    const char* name = job->name;
    if (flen == 14 && !strncmp(job->stored, "img/characters", 14)) {
        const char* base = name ? strrchr(name, '/') : NULL;
        base = base ? base + 1 : name;
        int big = base && (base[0] == '$' || (base[0] && base[1] == '$'));
        cx = big ? 3 : 12;
        cy = big ? 4 : 8;
    } else if (flen == 10 && !strncmp(job->stored, "img/system", 10) && name) {
        if (!strcmp(name, "Balloon")) {
            cx = 8;
            cy = 15;
        } else if (!strcmp(name, "Window")) {
            cx = 8;
            cy = 8;
        } else if (!strcmp(name, "IconSet")) {
            int w, h;
            if (png_size(&b, &w, &h) && w / 32 > 0 && h / 32 > 0) {
                cx = w / 32;
                cy = h / 32;
            }
        }
    }
    if (!tex_from_png(b.data, b.len, cx, cy, &job->entry->data)) job->entry->error = str_fmt("%s: could not convert", job->stored);
    buf_free(&b);
}

static char* stem_of(const char* stored) {
    const char* slash = strrchr(stored, '/');
    const char* base = slash ? slash + 1 : stored;
    const char* dot = strrchr(base, '.');
    return dot ? xstrndup(base, (size_t)(dot - base)) : xstrdup(base);
}

static char* out_name(const char* stored, const char* ext) {
    const char* slash = strrchr(stored, '/');
    char* stem = stem_of(stored);
    char* r = str_fmt("%.*s/%s.%s", (int)(slash - stored), stored, stem, ext);
    free(stem);
    return r;
}

static int build_images(Game* g, Node* actors, Pack* pack, int jobs) {
    Registry reg = {0};
    build_registry(g, actors, &reg);
    const char* folders[] = {"img/characters", "img/faces", "img/parallaxes", "img/pictures", "img/system", "img/titles1", NULL};
    char* keys = game_resolve(g, "img/pictures/keys.png");
    ImageJob* list = NULL;
    int n = 0, cap = 0;
    for (int f = 0; folders[f]; f++) {
        int cnt;
        char** files = game_stored_files(g, folders[f], &cnt);
        for (int i = 0; i < cnt; i++) {
            if (keys && !strcmp(files[i], keys)) continue;
            if (n == cap) {
                cap = cap ? cap * 2 : 512;
                list = xrealloc(list, sizeof(ImageJob) * (size_t)cap);
            }
            char* name = out_name(files[i], "tex");
            list[n].game = g;
            list[n].stored = xstrdup(files[i]);
            list[n].name = reg_get(&reg, files[i]);
            list[n].entry = NULL;
            free(name);
            n++;
        }
        str_list_free(files, cnt);
    }
    for (int i = 0; i < n; i++) {
        char* name = out_name(list[i].stored, "tex");
        list[i].entry = pack_add(pack, name);
        free(name);
    }
    int pictures = 0;
    if (keys) {
        Buf kb = {0};
        Buf ttf = {0};
        char* ttf_path = game_abs(g, "fonts/Faustina-Regular.ttf");
        if (game_read_stored(g, keys, &kb) && file_read(ttf_path, &ttf)) {
            Blob pad = art_blob(g_target->id);
            for (int swap = 0; swap <= keys_swappable(); swap++) {
                Image* art = keys_build(kb.data, kb.len, ttf.data, ttf.len, pad.data, pad.size, swap);
                char* stored = swap ? hash_path("img/pictures/keys_swap.png") : xstrdup(keys);
                char* name = out_name(stored, "tex");
                PackEntry* e = pack_add(pack, name);
                free(name);
                free(stored);
                if (!art || !tex_from_image(art, 0, 0, &e->data)) e->error = xstrdup("controls picture: could not build");
                img_free(art);
                pictures++;
            }
        } else {
            sl_add(&g_errors, xstrdup("controls picture: missing source"));
        }
        free(ttf_path);
        buf_free(&kb);
        buf_free(&ttf);
    }
    for (int i = 0; i < n; i++) list[i].entry = NULL;
    int base = pack->count - n - pictures;
    for (int i = 0; i < n; i++) list[i].entry = &pack->items[base + i];
    run_jobs(image_job, list, n, jobs);
    for (int i = 0; i < n; i++) free(list[i].stored);
    free(list);
    free(keys);
    for (int i = 0; i < reg.count; i++) {
        free(reg.stored[i]);
        free(reg.names[i]);
    }
    free(reg.stored);
    free(reg.names);
    return n + pictures;
}

typedef enum { AUDIO_BGM, AUDIO_BGS, AUDIO_ME, AUDIO_SE } AudioKind;

typedef struct {
    Game* game;
    char* stored;
    PackEntry* entry;
    AudioKind kind;
} AudioJob;

static void audio_job(void* ctx, int index) {
    AudioJob* job = &((AudioJob*)ctx)[index];
    Buf ogg = {0};
    int ok = game_read_stored(job->game, job->stored, &ogg);
    switch (job->kind) {
    case AUDIO_BGM: ok = ok && adpcm_stream_from_ogg(ogg.data, ogg.len, 32000, 2, 1, &job->entry->data); break;
    case AUDIO_BGS: ok = ok && adpcm_stream_from_ogg(ogg.data, ogg.len, 22050, 2, 1, &job->entry->data); break;
    case AUDIO_ME: ok = ok && adpcm_stream_from_ogg(ogg.data, ogg.len, 32000, 2, 0, &job->entry->data); break;
    case AUDIO_SE: ok = ok && adpcm_sample_from_ogg(ogg.data, ogg.len, 22050, &job->entry->data); break;
    }
    if (!ok) job->entry->error = str_fmt("%s: could not convert", job->stored);
    buf_free(&ogg);
}

static int build_ps1_audio(Game* g, Pack* pack, int jobs) {
    static const char* const FOLDERS[] = {"audio/bgm", "audio/bgs", "audio/me", "audio/se"};
    AudioJob* list = NULL;
    int n = 0;
    for (int k = 0; k < 4; k++) {
        int cnt;
        char** files = game_stored_files(g, FOLDERS[k], &cnt);
        list = xrealloc(list, sizeof(AudioJob) * (size_t)(n + cnt + 1));
        for (int i = 0; i < cnt; i++) {
            char* name = out_name(files[i], k == AUDIO_SE ? "vag" : "str");
            list[n] = (AudioJob){g, xstrdup(files[i]), NULL, (AudioKind)k};
            pack_add(pack, name);
            free(name);
            n++;
        }
        str_list_free(files, cnt);
    }
    int base = pack->count - n;
    for (int i = 0; i < n; i++) list[i].entry = &pack->items[base + i];
    run_jobs(audio_job, list, n, jobs);
    for (int i = 0; i < n; i++) free(list[i].stored);
    free(list);
    return n;
}

static int build_audio(Game* g, Pack* pack) {
    const char* folders[] = {"audio/bgm", "audio/bgs", "audio/me", "audio/se", NULL};
    int total = 0;
    for (int f = 0; folders[f]; f++) {
        int cnt;
        char** files = game_stored_files(g, folders[f], &cnt);
        for (int i = 0; i < cnt; i++) {
            char* name = out_name(files[i], "ogg");
            PackEntry* e = pack_add(pack, name);
            free(name);
            if (!game_read_stored(g, files[i], &e->data)) e->error = str_fmt("%s: unreadable", files[i]);
            total++;
        }
        str_list_free(files, cnt);
    }
    return total;
}

typedef struct {
    uint8_t* seen;
} CharSet;

static void cs_add_utf8(CharSet* cs, const char* s) {
    const unsigned char* p = (const unsigned char*)s;
    while (*p) {
        uint32_t cp;
        int n;
        if (*p < 0x80) {
            cp = *p;
            n = 1;
        } else if ((*p >> 5) == 6) {
            cp = *p & 0x1F;
            n = 2;
        } else if ((*p >> 4) == 14) {
            cp = *p & 0x0F;
            n = 3;
        } else if ((*p >> 3) == 30) {
            cp = *p & 0x07;
            n = 4;
        } else {
            p++;
            continue;
        }
        int ok = 1;
        for (int k = 1; k < n; k++) {
            if ((p[k] & 0xC0) != 0x80) {
                ok = 0;
                break;
            }
            cp = (cp << 6) | (p[k] & 0x3F);
        }
        if (!ok) {
            p++;
            continue;
        }
        p += n;
        if (cp < 0x110000) cs->seen[cp] = 1;
    }
}

static void cs_add_node(CharSet* cs, const Node* v) {
    if (!v) return;
    if (v->type == N_STR) cs_add_utf8(cs, v->str);
    else if (v->type == N_NUM) {
        char tmp[64];
        if (v->num == floor(v->num) && fabs(v->num) < 1e15) snprintf(tmp, sizeof(tmp), "%.0f", v->num);
        else snprintf(tmp, sizeof(tmp), "%.17g", v->num);
        cs_add_utf8(cs, tmp);
    } else if (v->type == N_ARR || v->type == N_OBJ) {
        for (size_t i = 0; i < v->count; i++) cs_add_node(cs, v->items[i]);
    } else if (v->type == N_TRUE) cs_add_utf8(cs, "True");
    else if (v->type == N_FALSE) cs_add_utf8(cs, "False");
    else if (v->type == N_NULL) cs_add_utf8(cs, "None");
}

typedef struct {
    const uint8_t* ttf;
    size_t ttf_len;
    uint32_t* cps;
    int ncps;
    PackEntry** tex;
    PackEntry** fnt;
    int* ok;
} FontJobs;

static void font_job(void* ctx, int i) {
    FontJobs* fj = ctx;
    fj->ok[i] = font_build(fj->ttf, fj->ttf_len, FONT_SIZES[i], fj->cps, fj->ncps, &fj->tex[i]->data, &fj->fnt[i]->data);
}

static void build_fonts(Game* g, Node* lang, const char* credits, Pack* pack, int jobs) {
    Buf ttf = {0};
    char* p = game_abs(g, "fonts/Faustina-Regular.ttf");
    if (!file_read(p, &ttf)) fatal("missing fonts/Faustina-Regular.ttf");
    free(p);
    CharSet cs;
    cs.seen = xcalloc(0x110000, 1);
    for (int c = 0x20; c < 0x7F; c++) cs.seen[c] = 1;
    Node* lines = node_get(lang, "lines");
    for (size_t i = 0; lines && i < lines->count; i++) {
        Node* v = lines->items[i];
        if (v->type == N_ARR) {
            for (size_t k = 0; k < v->count; k++) cs_add_node(&cs, v->items[k]);
        } else {
            cs_add_node(&cs, v);
        }
    }
    const char* groups[] = {"labels", "menus", "sys", NULL};
    for (int gi = 0; groups[gi]; gi++) {
        Node* grp = node_get(lang, groups[gi]);
        for (size_t i = 0; grp && i < grp->count; i++) cs_add_node(&cs, grp->items[i]);
    }
    cs_add_utf8(&cs, credits);
    cs_add_utf8(&cs, "\xE2\x80\xA6\xE2\x80\x94\xE2\x80\x99\xE2\x80\x98\xE2\x80\x9C\xE2\x80\x9D\xC3\xA9\xC3\x97");
    uint32_t* cps = NULL;
    int n = 0;
    for (uint32_t c = 0x20; c < 0x110000; c++) {
        if (!cs.seen[c] || c == 0x7F) continue;
        cps = xrealloc(cps, sizeof(uint32_t) * (size_t)(n + 1));
        cps[n++] = c;
    }
    free(cs.seen);
    int nsizes = (int)(sizeof(FONT_SIZES) / sizeof(FONT_SIZES[0]));
    FontJobs fj;
    fj.ttf = ttf.data;
    fj.ttf_len = ttf.len;
    fj.cps = cps;
    fj.ncps = n;
    fj.tex = xmalloc(sizeof(PackEntry*) * (size_t)nsizes);
    fj.fnt = xmalloc(sizeof(PackEntry*) * (size_t)nsizes);
    fj.ok = xcalloc((size_t)nsizes, sizeof(int));
    int first = pack->count;
    for (int i = 0; i < nsizes; i++) {
        char* a = str_fmt("fonts/game_%d.tex", FONT_SIZES[i]);
        char* b = str_fmt("fonts/game_%d.fnt", FONT_SIZES[i]);
        pack_add(pack, a);
        pack_add(pack, b);
        free(a);
        free(b);
    }
    for (int i = 0; i < nsizes; i++) {
        fj.tex[i] = &pack->items[first + i * 2];
        fj.fnt[i] = &pack->items[first + i * 2 + 1];
    }
    run_jobs(font_job, &fj, nsizes, jobs);
    for (int i = 0; i < nsizes; i++) {
        if (!fj.ok[i]) fj.tex[i]->error = str_fmt("font %d: could not build", FONT_SIZES[i]);
    }
    free(fj.tex);
    free(fj.fnt);
    free(fj.ok);
    free(cps);
    buf_free(&ttf);
}

static Image* fit_rgb(const Image* src, int w, int h) {
    Image* rgb = img_clone(src);
    for (int i = 0; i < rgb->w * rgb->h; i++) rgb->px[i * 4 + 3] = 255;
    Image* out = img_resize(rgb, w, h, FILTER_LANCZOS);
    img_free(rgb);
    return out;
}

static Image* cover_at(const Image* img, int w, int h, double fy) {
    double target = (double)w / h, aspect = (double)img->w / img->h;
    int x0 = 0, y0 = 0, x1 = img->w, y1 = img->h;
    if (aspect > target) {
        int nw = (int)(img->h * target + 0.5);
        x0 = (img->w - nw) / 2;
        x1 = x0 + nw;
    } else if (aspect < target) {
        int nh = (int)(img->w / target + 0.5);
        y0 = (int)((img->h - nh) * fy + 0.5);
        y1 = y0 + nh;
    }
    Image* cropped = img_crop(img, x0, y0, x1, y1);
    Image* out = fit_rgb(cropped, w, h);
    img_free(cropped);
    return out;
}

static Image* cover(const Image* img, int w, int h) { return cover_at(img, w, h, 0.5); }

static Image* load_title(Game* g) {
    Node* system = game_json(g, "data/System.json");
    const char* title = node_strv(node_get(system, "title1Name"));
    char* path = str_fmt("img/titles1/%s.png", title ? title : "");
    node_free(system);
    Buf b = {0};
    int ok = game_read(g, path, &b);
    free(path);
    if (!ok) {
        log_msg("  warning: could not build icons: title image missing");
        return NULL;
    }
    Image* img = img_load_png(b.data, b.len);
    buf_free(&b);
    if (!img) log_msg("  warning: could not build icons: bad title image");
    return img;
}

static int logo_strength(const uint8_t* p) {
    int r = p[0], g = p[1], b = p[2];
    int rest = g > b ? g : b;
    int v = (r - rest - 70) * 255 / 90;
    if (r < 150) return 0;
    return v < 0 ? 0 : v > 255 ? 255 : v;
}

static Image* logo_icon(const Image* title, int w, int h) {
    int x0 = title->w, y0 = title->h, x1 = -1, y1 = -1;
    long count = 0;
    for (int y = 0; y < title->h * 2 / 5; y++) {
        for (int x = title->w / 4; x < title->w * 3 / 4; x++) {
            if (logo_strength(title->px + ((size_t)y * title->w + x) * 4) < 128) continue;
            if (x < x0) x0 = x;
            if (x > x1) x1 = x;
            if (y < y0) y0 = y;
            if (y > y1) y1 = y;
            count++;
        }
    }
    if (count < 500 || x1 - x0 < 40 || y1 - y0 < 20) return NULL;
    int frame = h / 40 > 1 ? h / 40 : 1;
    int margin = frame * 2 + h / 8;
    int bw = x1 - x0 + 1, bh = y1 - y0 + 1;
    int lw = w - margin * 2, lh = bh * lw / bw;
    if (lh > h - margin * 2) {
        lh = h - margin * 2;
        lw = bw * lh / bh;
    }
    int grow = bw / lw / 2;
    if (grow < 1) grow = 1;
    int pad = grow;
    int cw = bw + pad * 2, ch = bh + pad * 2;
    uint8_t* mask = xcalloc((size_t)cw * ch, 1);
    long sum[3] = {0, 0, 0}, strong = 0;
    for (int y = 0; y < bh; y++) {
        for (int x = 0; x < bw; x++) {
            const uint8_t* p = title->px + ((size_t)(y0 + y) * title->w + x0 + x) * 4;
            int m = logo_strength(p);
            mask[(size_t)(y + pad) * cw + x + pad] = (uint8_t)m;
            if (m >= 128) {
                for (int c = 0; c < 3; c++) sum[c] += p[c];
                strong++;
            }
        }
    }
    uint8_t red[3];
    for (int c = 0; c < 3; c++) red[c] = (uint8_t)(sum[c] / strong);
    Image* logo = img_new(cw, ch);
    for (int y = 0; y < ch; y++) {
        for (int x = 0; x < cw; x++) {
            int m = 0;
            for (int dy = -grow; dy <= grow; dy++) {
                int yy = y + dy;
                if (yy < 0 || yy >= ch) continue;
                for (int dx = -grow; dx <= grow; dx++) {
                    int xx = x + dx;
                    if (xx < 0 || xx >= cw || dx * dx + dy * dy > grow * grow) continue;
                    int v = mask[(size_t)yy * cw + xx];
                    if (v > m) m = v;
                }
            }
            uint8_t* p = logo->px + ((size_t)y * cw + x) * 4;
            for (int c = 0; c < 3; c++) p[c] = (uint8_t)(red[c] * m / 255);
            p[3] = 255;
        }
    }
    free(mask);
    Image* small = img_resize(logo, lw, lh, FILTER_LANCZOS);
    img_free(logo);
    Image* icon = img_new(w, h);
    img_fill_rect(icon, 0, 0, w, h, 0x000000ffu);
    img_paste(icon, small, (w - lw) / 2, (h - lh) / 2);
    img_free(small);
    img_fill_rect(icon, frame, frame, w - frame, frame * 2, 0xffffffffu);
    img_fill_rect(icon, frame, h - frame * 2, w - frame, h - frame, 0xffffffffu);
    img_fill_rect(icon, frame, frame, frame * 2, h - frame, 0xffffffffu);
    img_fill_rect(icon, w - frame * 2, frame, w - frame, h - frame, 0xffffffffu);
    return icon;
}

static void psp_icons(const Image* img, Buf* icon0, Buf* pic1) {
    Image* bg = fit_rgb(img, 480, 272);
    png_encode(bg, 0, pic1);
    img_free(bg);
    Image* logo = logo_icon(img, 144, 80);
    if (logo) {
        png_encode(logo, 0, icon0);
        img_free(logo);
        return;
    }
    const Image* src = img;
    Image* cropped = NULL;
    double target = 144.0 / 80.0;
    if ((double)img->w / img->h > target) {
        int nw = (int)(img->h * target);
        int x0 = (img->w - nw) / 2;
        cropped = img_crop(img, x0, 0, x0 + nw, img->h);
        src = cropped;
    }
    Image* icon = fit_rgb(src, 144, 80);
    png_encode(icon, 0, icon0);
    img_free(icon);
    img_free(cropped);
}

static void ps3_icons(const Image* img, Buf* icon0, Buf* pic1) {
    Image* bg = cover(img, 1920, 1080);
    png_encode(bg, 0, pic1);
    img_free(bg);
    Image* icon = logo_icon(img, 320, 176);
    if (!icon) icon = cover(img, 320, 176);
    png_encode(icon, 0, icon0);
    img_free(icon);
}

static void vita_icons(const Image* img, Vpk* v) {
    Image* icon = cover(img, 128, 128);
    Image* bg = cover(img, 840, 500);
    Image* gate = cover(img, 280, 158);
    if (!png_encode_indexed(icon, &v->icon0) || !png_encode_indexed(bg, &v->bg) || !png_encode_indexed(gate, &v->startup)) {
        log_msg("  warning: could not build the LiveArea pictures");
        buf_free(&v->icon0);
        buf_free(&v->bg);
        buf_free(&v->startup);
    }
    img_free(icon);
    img_free(bg);
    img_free(gate);
}

static void wii_icon(const Image* img, Buf* png) {
    Image* icon = logo_icon(img, 128, 48);
    if (!icon) icon = cover_at(img, 128, 48, 0.1);
    png_encode(icon, 0, png);
    img_free(icon);
}

static void wiiu_icons(const Image* img, Wuhb* w) {
    Image* icon = cover(img, 128, 128);
    Image* tv = cover(img, 1280, 720);
    Image* drc = cover(img, 854, 480);
    if (!wuhb_tga_gz(icon, &w->icon) || !wuhb_tga_gz(tv, &w->tv) || !wuhb_tga_gz(drc, &w->drc)) {
        log_msg("  warning: could not build the Wii U Menu pictures");
        buf_free(&w->icon);
        buf_free(&w->tv);
        buf_free(&w->drc);
    }
    img_free(icon);
    img_free(tv);
    img_free(drc);
}

static void ctr_icon(const Image* title, Buf* icon) {
    int side = title->w < title->h ? title->w : title->h;
    int x0 = (title->w - side) / 2, y0 = (title->h - side) / 2;
    Image* square = img_crop(title, x0, y0, x0 + side, y0 + side);
    const int sizes[2] = {24, 48};
    for (int i = 0; i < 2; i++) {
        Image* small = fit_rgb(square, sizes[i], sizes[i]);
        ctr_icon_encode(small->px, sizes[i], icon);
        img_free(small);
    }
    img_free(square);
}

static int read_title_music(Game* g, Buf* ogg) {
    Node* system = game_json(g, "data/System.json");
    const char* name = node_strv(node_get(node_get(system, "titleBgm"), "name"));
    char* path = name && *name ? str_fmt("audio/bgm/%s.ogg", name) : NULL;
    node_free(system);
    int ok = path && game_read(g, path, ogg);
    free(path);
    return ok;
}

static void ctr_sound(Game* g, Pack* pack) {
    Buf ogg = {0}, cwav = {0};
    if (!read_title_music(g, &ogg)) log_msg("  warning: no HOME menu sound: title music missing");
    else if (!cwav_from_ogg(ogg.data, ogg.len, BANNER_SECONDS, &cwav)) log_msg("  warning: no HOME menu sound: could not decode the title music");
    else buf_put(&pack_add(pack, CTR_SOUND_NAME)->data, cwav.data, cwav.len);
    buf_free(&ogg);
    buf_free(&cwav);
}

static void ctr_banner(const Image* title, Buf* out) {
    int w = title->w, h = title->w * CTR_BANNER_H / CTR_BANNER_W;
    if (h > title->h) {
        h = title->h;
        w = title->h * CTR_BANNER_W / CTR_BANNER_H;
    }
    int x0 = (title->w - w) / 2, y0 = (title->h - h) / 2;
    Image* crop = img_crop(title, x0, y0, x0 + w, y0 + h);
    Image* small = fit_rgb(crop, CTR_BANNER_W, CTR_BANNER_H);
    ctr_banner_encode(small->px, out);
    img_free(small);
    img_free(crop);
}

static void ps2_save_icon(Game* g, Pack* pack) {
    Buf b = {0};
    Image* img = NULL;
    if (game_read(g, "icon/icon.png", &b)) img = img_load_png(b.data, b.len);
    buf_free(&b);
    if (!img) log_msg("  note: no game icon found, the memory card save gets a plain icon");
    ps2_icon_sys(TITLE, &pack_add(pack, "ps2/icon.sys")->data);
    ps2_icon_icn(img, &pack_add(pack, "ps2/icon.icn")->data);
    img_free(img);
}

static int is_supported(const char* v) {
    for (int i = 0; SUPPORTED[i]; i++) {
        if (!strcmp(SUPPORTED[i], v)) return 1;
    }
    return 0;
}

static int ends_with_ci(const char* s, const char* suf) {
    size_t a = strlen(s), b = strlen(suf);
    if (a < b) return 0;
    for (size_t i = 0; i < b; i++) {
        char x = s[a - b + i], y = suf[i];
        if (x >= 'A' && x <= 'Z') x = (char)(x + 32);
        if (y >= 'A' && y <= 'Z') y = (char)(y + 32);
        if (x != y) return 0;
    }
    return 1;
}

static char* parent_dir(const char* path) {
    const char* a = strrchr(path, '/');
    const char* b = strrchr(path, '\\');
    const char* s = a > b ? a : b;
    return s ? xstrndup(path, (size_t)(s - path)) : xstrdup(".");
}

static char g_ui_title[64] = "The Coffin of Andy and Leyley builder";
#define UI_TITLE g_ui_title
static const char* PS3_EBOOT_NAME = "USRDIR/EBOOT.BIN";
static const char* PS3_PAK_NAME = "USRDIR/TCOAAL.PAK";

static void use_target(const Target* t) {
    target_set(t);
    snprintf(g_ui_title, sizeof(g_ui_title), "The Coffin of Andy and Leyley for %s", t->console);
}

static void target_names(char* out, size_t cap) {
    out[0] = 0;
    for (int i = 0; i < TARGET_COUNT; i++) {
        size_t n = strlen(out);
        snprintf(out + n, cap - n, "%s%s%s", i ? (i + 1 == TARGET_COUNT ? " or " : ", ") : "", target_get((TargetId)i)->name, i ? "" : " (default)");
    }
}

static void usage(void) {
    char names[96];
    target_names(names, sizeof(names));
    printf("usage: tcoaal-builder [game folder] [options]\n"
           "\n"
           "Builds The Coffin of Andy and Leyley for a console from your own copy of the game.\n"
           "Run it without a game folder to find your Steam copy automatically and pick the console and folders in windows.\n"
           "\n"
           "options:\n"
           "  -t, --target NAME      console to build for: %s\n"
           "  -o, --out PATH         output folder (default: out); the console's own folder layout is created inside it.\n"
           "                         For psp, 3ds, ps2, ps3 and ps1, a path ending in .PBP, .3dsx, .iso, .pkg or .cue (with the .bin next to it)\n"
           "                         writes there instead\n"
           "  --engine FILE          use this engine file instead of the built-in one\n"
           "  --pak FILE             also write the data pack on its own (for the desktop test build; not for ps1)\n"
           "  --voices DIR           add voice clips (DIR/**/<line id>.ogg) for the dialogue\n"
           "  --jobs N               worker threads (default: all cores)\n"
           "  --allow-any-version    convert even if the game version is not supported\n"
           "  --cia                  3ds: also write tcoaal.cia, which installs to the HOME menu with FBI\n"
           "  --make-cia FILE        3ds: turn an existing tcoaal.3dsx into tcoaal.cia (-o sets the output) and exit\n"
           "  --folder DIR           ps2: also write TCOAAL.ELF and TCOAAL.PAK to DIR, to start from USB with uLaunchELF\n"
           "  --update-engine FILE   replace only the engine inside an existing build (EBOOT.PBP, TCOAAL.vpk, tcoaal.3dsx, boot.dol,\n"
           "                         tcoaal.wuhb, TCOAAL.iso, TCOAAL.pkg, xenon.elf or TCOAAL.cue) and exit\n",
           names);
}

static uint32_t rd_be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

static int dol_check(const uint8_t* d, size_t len) {
    if (len < 0x100 || !rd_be32(d + 0x90)) return 0;
    uint32_t entry = rd_be32(d + 0xE0);
    if (entry < 0x80000000u || entry >= 0x81800000u) return 0;
    for (int i = 0; i < 18; i++) {
        uint32_t off = rd_be32(d + i * 4), size = rd_be32(d + 0x90 + i * 4);
        if (size && (off < 0x100 || off > len || size > len - off)) return 0;
    }
    return 1;
}

static int ppc_elf(const uint8_t* d, size_t len) { return len >= 52 && !memcmp(d, "\x7f" "ELF", 4) && d[4] == 1 && d[5] == 2 && d[18] == 0 && d[19] == 20; }

static int rpx_check(const uint8_t* d, size_t len) { return ppc_elf(d, len) && d[7] == 0xCA && d[8] == 0xFE; }

static int xenon_check(const uint8_t* d, size_t len) { return ppc_elf(d, len) && d[7] != 0xCA; }

static int engine_valid(TargetId id, const uint8_t* data, size_t len) {
    switch (id) {
        case TARGET_VITA:
            return len >= 4 && !memcmp(data, "SCE\0", 4);
        case TARGET_3DS:
            return ctr_check(data, len);
        case TARGET_WII:
            return dol_check(data, len);
        case TARGET_WIIU:
            return rpx_check(data, len);
        case TARGET_PS2:
            return elf_check(data, len);
        case TARGET_PS3:
            return self_valid(data, len);
        case TARGET_XENON:
            return xenon_check(data, len);
        case TARGET_PS1:
            return exe_check(data, len);
        default: {
            Pbp p;
            int ok = pbp_read(data, len, &p);
            pbp_free(&p);
            return ok;
        }
    }
}

static int has_engine(TargetId id) {
    Blob b = engine_blob(id);
    return b.size > 0;
}

static int load_engine(const char* path, Buf* out) {
    if (path) {
        if (!file_read(path, out)) {
            fprintf(stderr, "error: cannot read %s\n", path);
            return 0;
        }
        if (!engine_valid(g_target->id, out->data, out->len)) {
            fprintf(stderr, "error: %s is not a %s engine\n", path, g_target->console);
            buf_free(out);
            return 0;
        }
        return 1;
    }
    Blob b = engine_blob(g_target->id);
    if (!b.size) {
        fprintf(stderr, "error: this builder was compiled without the %s engine\n", g_target->console);
        return 0;
    }
    if (!engine_valid(g_target->id, b.data, b.size)) {
        fprintf(stderr, "error: built-in %s engine is damaged\n", g_target->console);
        return 0;
    }
    buf_put(out, b.data, b.size);
    return 1;
}

static int load_vita_sfo(const char* engine_path, Buf* out) {
    if (engine_path) {
        char* dir = parent_dir(engine_path);
        char* side = path_join(dir, "param.sfo");
        int own = file_read(side, out);
        if (own && (out->len < 4 || memcmp(out->data, "\0PSF", 4) != 0)) {
            fprintf(stderr, "error: %s is not a param.sfo\n", side);
            buf_free(out);
            own = -1;
        }
        free(side);
        free(dir);
        if (own) return own > 0;
    }
    if (!embedded_vita_sfo_size) {
        fprintf(stderr, "error: this builder was compiled without the PS Vita param.sfo; put param.sfo next to the --engine file\n");
        return 0;
    }
    buf_put(out, embedded_vita_sfo, embedded_vita_sfo_size);
    return 1;
}

static int pak_version(const uint8_t* pak, size_t len) {
    if (len < 8 || memcmp(pak, "TPAK", 4) != 0) return -1;
    return (int)(pak[4] | pak[5] << 8 | pak[6] << 16 | (uint32_t)pak[7] << 24);
}

static int stale(const char* target, int version) {
    if (version == PACK_VERSION) return 0;
    fprintf(stderr, "error: %s was made by an older builder, build it again from the game instead\n", target);
    return 1;
}

static int update_psp(const char* target, Buf* cur, const char* engine_path, Buf* out) {
    Pbp pbp, eng;
    if (!pbp_read(cur->data, cur->len, &pbp)) {
        fprintf(stderr, "error: %s is not an EBOOT.PBP\n", target);
        return 0;
    }
    Buf e = {0};
    int ok = !stale(target, pak_version(pbp.part[7].data, pbp.part[7].len)) && load_engine(engine_path, &e) && pbp_read(e.data, e.len, &eng);
    buf_free(&e);
    if (ok) {
        buf_free(&pbp.part[6]);
        pbp.part[6] = eng.part[6];
        memset(&eng.part[6], 0, sizeof(Buf));
        pbp_free(&eng);
        pbp_write(&pbp, out);
    }
    pbp_free(&pbp);
    return ok;
}

static int update_3ds(const char* target, Buf* cur, const char* engine_path, Buf* out) {
    Buf icon = {0}, pak = {0}, eng = {0};
    if (!ctr_unpack(cur->data, cur->len, &icon, &pak)) {
        fprintf(stderr, "error: %s is not a tcoaal.3dsx made by this builder\n", target);
        return 0;
    }
    int ok = !stale(target, pak_version(pak.data, pak.len)) && load_engine(engine_path, &eng) && ctr_build(eng.data, eng.len, icon.data, &pak, out);
    buf_free(&eng);
    buf_free(&icon);
    buf_free(&pak);
    return ok;
}

static int update_ps2(const char* target, Buf* cur, const char* engine_path, Buf* out) {
    size_t off, size;
    if (!iso_find(cur->data, cur->len, PS2_PAK_NAME, &off, &size)) {
        fprintf(stderr, "error: %s is not a TCOAAL.iso made by this builder\n", target);
        return 0;
    }
    Buf eng = {0};
    int ok = !stale(target, pak_version(cur->data + off, size)) && load_engine(engine_path, &eng) && ps2_iso_build(eng.data, eng.len, cur->data + off, size, out);
    buf_free(&eng);
    return ok;
}

static int data_stale(const char* target, const char* rel) {
    char* dir = parent_dir(target);
    char* pak = path_join(dir, rel);
    FILE* f = file_open(pak, "rb");
    int bad = 0;
    if (f) {
        uint8_t head[8];
        size_t n = fread(head, 1, sizeof(head), f);
        fclose(f);
        bad = stale(pak, pak_version(head, n));
    }
    free(pak);
    free(dir);
    return bad;
}

static int update_vita(const char* target, Buf* cur, const char* engine_path, Buf* out) {
    Buf eng = {0};
    int ok = !data_stale(target, "data/TCOAAL/tcoaal.pak") && load_engine(engine_path, &eng);
    if (ok && !vpk_replace_eboot(cur->data, cur->len, eng.data, eng.len, out)) {
        fprintf(stderr, "error: cannot rebuild %s\n", target);
        ok = 0;
    }
    buf_free(&eng);
    return ok;
}

static int update_wiiu(const char* target, Buf* cur, const char* engine_path, Buf* out) {
    Buf eng = {0};
    int ok = !data_stale(target, "tcoaal.pak") && load_engine(engine_path, &eng);
    if (ok && !wuhb_replace_rpx(cur->data, cur->len, eng.data, eng.len, out)) {
        fprintf(stderr, "error: %s is not a tcoaal.wuhb made by this builder\n", target);
        ok = 0;
    }
    buf_free(&eng);
    return ok;
}

static int update_plain(const char* target, const char* rel, const char* engine_path, Buf* out) {
    return !data_stale(target, rel) && load_engine(engine_path, out);
}

static int update_ps3(const char* target, Buf* cur, const char* engine_path, int jobs) {
    PkgFile* files = NULL;
    int count = 0;
    if (!pkg_read(cur->data, cur->len, &files, &count, jobs)) {
        fprintf(stderr, "error: %s is not a TCOAAL.pkg\n", target);
        return 0;
    }
    buf_free(cur);
    int found = 0, version = -1;
    for (int i = 0; i < count; i++) {
        if (!strcmp(files[i].name, PS3_EBOOT_NAME)) found = 1;
        if (!strcmp(files[i].name, PS3_PAK_NAME)) version = pak_version(files[i].data.data, files[i].data.len);
    }
    Buf eng = {0};
    if (!found || stale(target, version) || !load_engine(engine_path, &eng)) {
        if (!found) fprintf(stderr, "error: %s has no EBOOT.BIN\n", target);
        pkg_files_free(files, count);
        return 0;
    }
    PkgItem* items = xcalloc((size_t)count + 1, sizeof(PkgItem));
    for (int i = 0; i < count; i++) {
        items[i].name = files[i].name;
        items[i].dir = files[i].dir;
        items[i].data = !strcmp(files[i].name, PS3_EBOOT_NAME) ? &eng : &files[i].data;
        items[i].size = items[i].dir ? 0 : items[i].data->len;
    }
    int ok = pkg_write(target, PKG_CONTENT_ID, items, count, jobs) > 0;
    if (!ok) fprintf(stderr, "error: cannot write %s\n", target);
    free(items);
    pkg_files_free(files, count);
    buf_free(&eng);
    return ok;
}

static int read_at(FILE* f, long off, size_t len, Buf* out) {
    out->len = 0;
    buf_zero(out, len);
    return fseek(f, off, SEEK_SET) == 0 && fread(out->data, 1, len, f) == len;
}

static char* bin_path_for(const char* cue) { return str_fmt("%.*s.bin", (int)(strlen(cue) - 4), cue); }

static int is_raw_disc(const char* path) {
    static const uint8_t SYNC[12] = {0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0};
    FILE* f = file_open(path, "rb");
    uint8_t head[12];
    int ok = f && fread(head, 1, sizeof(head), f) == sizeof(head) && !memcmp(head, SYNC, sizeof(SYNC));
    if (f) fclose(f);
    return ok;
}

static int update_ps1(const char* bin, const char* engine_path) {
    Buf exe = {0}, head = {0}, pak = {0};
    FILE* f = file_open(bin, "r+b");
    uint32_t lba = 0, size = 0, room = 0, pak_lba = 0, pak_size = 0, pak_room = 0;
    int ok = f && read_at(f, 0, (size_t)DISC_HEAD_SECTORS * DISC_RAW, &head) && disc_find(head.data, head.len, DISC_EXE, &lba, &size, &room) &&
             disc_find(head.data, head.len, DISC_PAK, &pak_lba, &pak_size, &pak_room);
    if (!ok) {
        fprintf(stderr, "error: %s is not a TCOAAL.bin made by this builder\n", bin);
    } else if (!read_at(f, (long)pak_lba * DISC_RAW + 24, 8, &pak) || pak_version(pak.data, pak.len) != PACK_DISC_VERSION) {
        fprintf(stderr, "error: %s was made by an older builder, build it again from the game instead\n", bin);
        ok = 0;
    } else {
        size_t span = (size_t)(lba + room) * DISC_RAW;
        ok = load_engine(engine_path, &exe) && read_at(f, 0, span, &head) && disc_replace(head.data, head.len, DISC_EXE, exe.data, exe.len) &&
             fseek(f, 0, SEEK_SET) == 0 && fwrite(head.data, 1, span, f) == span;
        if (!ok) fprintf(stderr, "error: cannot update the engine in %s\n", bin);
    }
    if (f && fclose(f) != 0) ok = 0;
    buf_free(&head);
    buf_free(&pak);
    buf_free(&exe);
    return ok;
}

static int update_engine(const char* target, const char* engine_path, int jobs) {
    char* bin = ends_with_ci(target, ".cue") ? bin_path_for(target) : xstrdup(target);
    if (is_raw_disc(bin)) {
        use_target(target_get(TARGET_PS1));
        int ok = update_ps1(bin, engine_path);
        if (ok) log_msg("engine updated in %s", bin);
        free(bin);
        return ok ? 0 : 2;
    }
    free(bin);
    Buf cur = {0}, out = {0};
    if (!file_read(target, &cur)) {
        fprintf(stderr, "error: cannot read %s\n", target);
        return 2;
    }
    size_t off, size;
    int ok;
    if (cur.len >= 4 && !memcmp(cur.data, "\0PBP", 4)) {
        use_target(target_get(TARGET_PSP));
        ok = update_psp(target, &cur, engine_path, &out);
    } else if (cur.len >= 4 && !memcmp(cur.data, "3DSX", 4)) {
        use_target(target_get(TARGET_3DS));
        ok = update_3ds(target, &cur, engine_path, &out);
    } else if (cur.len >= 4 && !memcmp(cur.data, "\x7FPKG", 4)) {
        use_target(target_get(TARGET_PS3));
        ok = update_ps3(target, &cur, engine_path, jobs);
        buf_free(&cur);
        if (!ok) return 2;
        log_msg("engine updated in %s", target);
        return 0;
    } else if (vpk_is_vpk(cur.data, cur.len)) {
        use_target(target_get(TARGET_VITA));
        ok = update_vita(target, &cur, engine_path, &out);
    } else if (wuhb_is_wuhb(cur.data, cur.len)) {
        use_target(target_get(TARGET_WIIU));
        ok = update_wiiu(target, &cur, engine_path, &out);
    } else if (ends_with_ci(target, ".dol") && dol_check(cur.data, cur.len)) {
        use_target(target_get(TARGET_WII));
        ok = update_plain(target, "tcoaal.pak", engine_path, &out);
    } else if (xenon_check(cur.data, cur.len)) {
        use_target(target_get(TARGET_XENON));
        ok = update_plain(target, "tcoaal/tcoaal.pak", engine_path, &out);
    } else if (iso_find(cur.data, cur.len, PS2_PAK_NAME, &off, &size)) {
        use_target(target_get(TARGET_PS2));
        ok = update_ps2(target, &cur, engine_path, &out);
    } else {
        fprintf(stderr, "error: %s is not an EBOOT.PBP, TCOAAL.vpk, tcoaal.3dsx, boot.dol, tcoaal.wuhb, TCOAAL.iso, TCOAAL.pkg, xenon.elf or TCOAAL.cue made by this builder\n", target);
        buf_free(&cur);
        return 2;
    }
    buf_free(&cur);
    if (!ok) {
        buf_free(&out);
        return 2;
    }
    ok = file_write(target, out.data, out.len);
    buf_free(&out);
    if (!ok) {
        fprintf(stderr, "error: cannot write %s\n", target);
        return 2;
    }
    log_msg("engine updated in %s", target);
    return 0;
}

static void fatal_dialog(const char* msg) {
    if (ui_is_native()) ui_message(UI_TITLE, msg, 1);
}

static int check_copy(Game* g) {
    char reason[512];
    char* inst = install_dir_of(g->root);
    int cracked = crack_scan(inst, reason, sizeof(reason));
    free(inst);
    if (!cracked) return 1;
    log_msg("this copy looks cracked: %s", reason);
    if (getenv("TCOAAL_CRACK_OK")) return 1;
    int native = ui_is_native();
    if (!native && getenv("TCOAAL_GUI") && ui_native_available()) ui_use_native(1);
    else if (!native && !ui_stdin_is_terminal()) {
        if (!ui_native_available()) {
            fprintf(stderr, "error: this copy of the game looks cracked (%s). Buy the game on Steam: %s\n", reason, STEAM_STORE_URL);
            return 0;
        }
        ui_use_native(1);
    }
    char* text = str_fmt("This copy of The Coffin of Andy and Leyley looks cracked.\nFound: %s\n\n"
                         "You need to buy the game to use this port. Kit9 Studio and Nemlei made it, and buying it on Steam "
                         "is what lets them keep making games.",
                         reason);
    CrackChoice c = ui_crack_warning("Please buy the game", text);
    free(text);
    ui_use_native(native);
    if (c == CRACK_BUY) ui_open_url(STEAM_STORE_URL);
    if (c != CRACK_CONTINUE) {
        log_msg("stopped");
        return 0;
    }
    return 1;
}

static char* choose_game(void) {
    char* found = steam_find_game();
    if (found) {
        char* q = str_fmt("Found The Coffin of Andy and Leyley in your Steam library:\n%s\n\nUse this copy?", found);
        int yes = ui_yes_no(UI_TITLE, q, "Use this copy", "Choose another folder");
        free(q);
        if (yes) return found;
        free(found);
    }
    const char* prompt = found ? "Select the game folder (the one with Game.exe in it)"
                               : "Steam copy not found. Select the game folder (the one with Game.exe in it)";
    for (;;) {
        char* dir = ui_pick_folder(UI_TITLE, prompt);
        if (!dir) return NULL;
        Game g;
        if (game_open(&g, dir)) {
            free(g.root);
            free(g.version);
            return dir;
        }
        ui_message(UI_TITLE, "That folder does not contain The Coffin of Andy and Leyley. Pick the folder that has Game.exe and the www folder in it.", 1);
        free(dir);
    }
}

typedef struct {
    const Target* target;
    const char* engine;
    const char* pak;
    const char* voices;
    const char* folder;
    int jobs;
    int any_version;
    int cia;
} Options;

static char* cia_path_for(const char* app) {
    size_t n = strlen(app);
    if (ends_with_ci(app, ".3dsx")) return str_fmt("%.*s.cia", (int)(n - 5), app);
    return str_fmt("%s.cia", app);
}

static int write_cia(const uint8_t* app, size_t len, const char* target) {
    size_t smdh, romfs;
    if (!ctr_parts(app, len, &smdh, &romfs)) {
        fprintf(stderr, "error: not a tcoaal.3dsx made by this builder\n");
        return 0;
    }
    if (ctr_pak_version(app, len) != PACK_VERSION) {
        fprintf(stderr, "error: this tcoaal.3dsx was made by an older builder, build it again from the game first\n");
        return 0;
    }
    if (!embedded_3ds_cia_size) {
        fprintf(stderr, "error: this builder was compiled without the CIA template\n");
        return 0;
    }
    log_msg("cia");
    CiaLook look = {app + smdh, NULL, NULL, 0};
    size_t off, size;
    if (ctr_pak_entry(app, len, CTR_BANNER_NAME, &off, &size) && size == CTR_BANNER_SIZE) look.banner = app + off;
    else log_msg("  note: no title art in this tcoaal.3dsx, so the HOME menu banner stays plain");
    if (ctr_pak_entry(app, len, CTR_SOUND_NAME, &off, &size) && size > 4 && !memcmp(app + off, "CWAV", 4)) {
        look.sound = app + off;
        look.sound_len = size;
    } else {
        log_msg("  note: no title music in this tcoaal.3dsx, so the HOME menu banner is silent");
    }
    Buf cia = {0};
    if (!cia_build(embedded_3ds_cia, embedded_3ds_cia_size, &look, app + romfs, len - romfs, &cia)) {
        fprintf(stderr, "error: built-in CIA template is damaged\n");
        return 0;
    }
    int ok = file_write(target, cia.data, cia.len);
    if (ok) log_msg("done (%.1f MB) -> %s", cia.len / 1048576.0, target);
    else fprintf(stderr, "error: cannot write %s\n", target);
    buf_free(&cia);
    return ok;
}

static int make_cia(const char* src, const char* dst) {
    Buf app = {0};
    if (!file_read(src, &app)) {
        fprintf(stderr, "error: cannot read %s\n", src);
        return 2;
    }
    char* target = dst ? xstrdup(dst) : cia_path_for(src);
    int ok = write_cia(app.data, app.len, target);
    free(target);
    buf_free(&app);
    return ok ? 0 : 2;
}

static int build_voices(const char* dir, Pack* pack) {
    int n;
    char** names = dir_list(dir, &n);
    int count = 0;
    for (int i = 0; i < n; i++) {
        char* path = path_join(dir, names[i]);
        if (dir_exists(path)) count += build_voices(path, pack);
        else if (ends_with_ci(names[i], ".ogg")) {
            char* name = str_fmt("vo/%s", names[i]);
            PackEntry* e = pack_add(pack, name);
            if (!file_read(path, &e->data)) e->error = str_fmt("%s: unreadable", path);
            free(name);
            count++;
        }
        free(path);
    }
    str_list_free(names, n);
    return count;
}

static char* output_path(const char* out) {
    switch (g_target->id) {
        case TARGET_VITA:
            return path_join(out, "TCOAAL.vpk");
        case TARGET_WII:
            return str_fmt("%s/apps/tcoaal/boot.dol", out);
        case TARGET_WIIU:
            return str_fmt("%s/wiiu/apps/tcoaal/tcoaal.wuhb", out);
        case TARGET_XENON:
            return path_join(out, "xenon.elf");
        case TARGET_3DS:
            return ends_with_ci(out, ".3dsx") ? xstrdup(out) : str_fmt("%s/3ds/tcoaal/tcoaal.3dsx", out);
        case TARGET_PS2:
            return ends_with_ci(out, ".iso") ? xstrdup(out) : path_join(out, "TCOAAL.iso");
        case TARGET_PS3:
            return ends_with_ci(out, ".pkg") ? xstrdup(out) : str_fmt("%s/TCOAAL.pkg", out);
        case TARGET_PS1:
            return ends_with_ci(out, ".cue") ? xstrdup(out) : path_join(out, "TCOAAL.cue");
        default:
            return ends_with_ci(out, ".pbp") ? xstrdup(out) : str_fmt("%s/PSP/GAME/TCOAAL/EBOOT.PBP", out);
    }
}

static char* data_path(const char* out) {
    switch (g_target->id) {
        case TARGET_VITA:
            return str_fmt("%s/data/TCOAAL/tcoaal.pak", out);
        case TARGET_WII:
            return str_fmt("%s/apps/tcoaal/tcoaal.pak", out);
        case TARGET_WIIU:
            return str_fmt("%s/wiiu/apps/tcoaal/tcoaal.pak", out);
        default:
            return str_fmt("%s/tcoaal/tcoaal.pak", out);
    }
}

static int write_pak(const char* path, const Buf* data) {
    char* dir = parent_dir(path);
    make_dirs(dir);
    free(dir);
    if (file_write(path, data->data, data->len)) return 1;
    fprintf(stderr, "error: cannot write %s\n", path);
    return 0;
}

static int pack_to(Pack* pack, const char* path) {
    char* dir = parent_dir(path);
    make_dirs(dir);
    free(dir);
    if (pack_write(pack, path)) return 1;
    fprintf(stderr, "error: cannot write %s\n", path);
    return 0;
}

static uint64_t write_data(Pack* pack, const Options* o, const char* out) {
    char* path = data_path(out);
    uint64_t size = pack_size(pack);
    int ok = pack_to(pack, path);
    if (ok) log_msg("  data -> %s", path);
    if (ok && o->pak) pack_to(pack, o->pak);
    free(path);
    return ok ? size : 0;
}

static const char WII_META[] = "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
                               "<app version=\"1\">\n"
                               "  <name>The Coffin of Andy and Leyley</name>\n"
                               "  <coder>kotwicha</coder>\n"
                               "  <version>3.0.12</version>\n"
                               "  <short_description>Unofficial port, built from your own copy</short_description>\n"
                               "  <long_description>An unofficial port of The Coffin of Andy and Leyley with an engine written from scratch. "
                               "The Coffin of Andy and Leyley is (c) Kit9 Studio / Nemlei. This project is not affiliated with or endorsed by them.</long_description>\n"
                               "</app>\n";

static uint64_t package_vita(Game* g, Pack* pack, Buf* eng, const Buf* sfo, const Options* o, const char* out, Buf* file) {
    log_msg("vpk");
    Vpk v;
    memset(&v, 0, sizeof(v));
    v.eboot = eng->data;
    v.eboot_len = eng->len;
    v.sfo = sfo->data;
    v.sfo_len = sfo->len;
    Image* title = load_title(g);
    if (title) {
        vita_icons(title, &v);
        img_free(title);
    }
    uint64_t size = write_data(pack, o, out);
    if (size && !vpk_build(&v, file)) size = 0;
    buf_free(&v.icon0);
    buf_free(&v.bg);
    buf_free(&v.startup);
    return size;
}

static uint64_t package_wii(Game* g, Pack* pack, Buf* eng, const Options* o, const char* out, Buf* file) {
    log_msg("homebrew channel app");
    char* meta = str_fmt("%s/apps/tcoaal/meta.xml", out);
    char* icon = str_fmt("%s/apps/tcoaal/icon.png", out);
    Buf png = {0};
    Image* title = load_title(g);
    if (title) {
        wii_icon(title, &png);
        img_free(title);
    }
    uint64_t size = 0;
    if (!file_write(meta, WII_META, sizeof(WII_META) - 1)) fprintf(stderr, "error: cannot write %s\n", meta);
    else if (png.len && !file_write(icon, png.data, png.len)) fprintf(stderr, "error: cannot write %s\n", icon);
    else size = write_data(pack, o, out);
    if (size) buf_put(file, eng->data, eng->len);
    buf_free(&png);
    free(meta);
    free(icon);
    return size;
}

static uint64_t package_wiiu(Game* g, Pack* pack, Buf* eng, const Options* o, const char* out, Buf* file) {
    log_msg("wuhb");
    Wuhb w;
    memset(&w, 0, sizeof(w));
    w.rpx = eng->data;
    w.rpx_len = eng->len;
    Image* title = load_title(g);
    if (title) {
        wiiu_icons(title, &w);
        img_free(title);
    }
    uint64_t size = write_data(pack, o, out);
    if (size) wuhb_build(&w, file);
    buf_free(&w.icon);
    buf_free(&w.tv);
    buf_free(&w.drc);
    return size;
}

static uint64_t package_xenon(Pack* pack, Buf* eng, const Options* o, const char* out, Buf* file) {
    log_msg("xell files");
    uint64_t size = write_data(pack, o, out);
    if (size) buf_put(file, eng->data, eng->len);
    return size;
}

static int package_psp(Game* g, Pack* pack, Buf* eng, const Options* o, Buf* file) {
    log_msg("eboot");
    Pbp e, out;
    if (!pbp_read(eng->data, eng->len, &e)) return 0;
    memset(&out, 0, sizeof(out));
    sfo_build(TITLE, &out.part[0]);
    Image* title = load_title(g);
    if (title) {
        psp_icons(title, &out.part[1], &out.part[4]);
        img_free(title);
    }
    out.part[6] = e.part[6];
    memset(&e.part[6], 0, sizeof(Buf));
    pbp_free(&e);
    pack_build(pack, &out.part[7]);
    if (o->pak) write_pak(o->pak, &out.part[7]);
    pbp_write(&out, file);
    pbp_free(&out);
    return 1;
}

static int package_3ds(Game* g, Pack* pack, Buf* eng, const Options* o, Buf* file) {
    log_msg("3dsx");
    Buf icon = {0}, data = {0};
    Image* title = load_title(g);
    if (title) {
        ctr_icon(title, &icon);
        ctr_banner(title, &pack_add(pack, CTR_BANNER_NAME)->data);
        img_free(title);
    }
    ctr_sound(g, pack);
    pack_build(pack, &data);
    if (o->pak) write_pak(o->pak, &data);
    int ok = ctr_build(eng->data, eng->len, icon.len == CTR_ICON_SIZE ? icon.data : NULL, &data, file);
    buf_free(&icon);
    buf_free(&data);
    return ok;
}

static int package_ps2(Game* g, Pack* pack, Buf* eng, const Options* o, Buf* file, int* failed) {
    log_msg("iso");
    ps2_save_icon(g, pack);
    Buf data = {0};
    pack_build(pack, &data);
    if (o->pak) write_pak(o->pak, &data);
    if (o->folder) {
        make_dirs(o->folder);
        char* elf_path = path_join(o->folder, "TCOAAL.ELF");
        char* pak_path = path_join(o->folder, PS2_PAK_NAME);
        if (!file_write(elf_path, eng->data, eng->len) || !file_write(pak_path, data.data, data.len)) {
            fprintf(stderr, "error: cannot write to %s\n", o->folder);
            (*failed)++;
        } else {
            log_msg("  folder -> %s", o->folder);
        }
        free(elf_path);
        free(pak_path);
    }
    int ok = ps2_iso_build(eng->data, eng->len, data.data, data.len, file);
    buf_free(&data);
    return ok;
}

static uint64_t package_ps3(Game* g, Pack* pack, Buf* eng, const Options* o, const char* target) {
    log_msg("package");
    Buf sfo = {0}, icon0 = {0}, pic1 = {0}, snd0 = {0}, ogg = {0};
    ps3_sfo_build(TITLE, PKG_TITLE_ID, &sfo);
    Image* title = load_title(g);
    if (title) {
        ps3_icons(title, &icon0, &pic1);
        img_free(title);
    }
    if (!read_title_music(g, &ogg)) log_msg("  warning: no XMB music: title music missing");
    else if (!snd0_from_ogg(ogg.data, ogg.len, &snd0)) log_msg("  warning: no XMB music: could not decode the title music");
    buf_free(&ogg);
    if (o->pak) {
        char* pak_dir = parent_dir(o->pak);
        make_dirs(pak_dir);
        free(pak_dir);
        if (!pack_write(pack, o->pak)) fprintf(stderr, "error: cannot write %s\n", o->pak);
    }
    PkgItem items[7];
    int n = 0;
    memset(items, 0, sizeof(items));
    if (icon0.len) items[n++] = (PkgItem){"ICON0.PNG", 0, icon0.len, &icon0, NULL, NULL};
    items[n++] = (PkgItem){"PARAM.SFO", 0, sfo.len, &sfo, NULL, NULL};
    if (pic1.len) items[n++] = (PkgItem){"PIC1.PNG", 0, pic1.len, &pic1, NULL, NULL};
    if (snd0.len) items[n++] = (PkgItem){"SND0.AT3", 0, snd0.len, &snd0, NULL, NULL};
    items[n++] = (PkgItem){"USRDIR", 1, 0, NULL, NULL, NULL};
    items[n++] = (PkgItem){PS3_EBOOT_NAME, 0, eng->len, eng, NULL, NULL};
    items[n++] = (PkgItem){PS3_PAK_NAME, 0, pack_size(pack), NULL, pack_stream, pack};
    uint64_t size = pkg_write(target, PKG_CONTENT_ID, items, n, o->jobs);
    buf_free(&sfo);
    buf_free(&icon0);
    buf_free(&pic1);
    buf_free(&snd0);
    return size;
}

static uint64_t package_ps1(Game* g, Pack* pack, Buf* eng, const char* target, int* failed) {
    log_msg("disc");
    Buf icon = {0};
    PackEntry* card = pack_add(pack, "card/icon.bin");
    if (!game_read(g, "icon/icon.png", &icon) || !psx_card_icon(icon.data, icon.len, &card->data)) {
        sl_add(&g_errors, xstrdup("icon/icon.png: could not convert"));
        (*failed)++;
    }
    buf_free(&icon);
    Buf pak = {0}, bin = {0};
    pack_build_disc(pack, &pak);
    int ok = ps1_disc_build(eng->data, eng->len, pak.data, pak.len, &bin);
    buf_free(&pak);
    char* bin_path = bin_path_for(target);
    const char* slash = strrchr(bin_path, '/');
    const char* back = strrchr(bin_path, '\\');
    if (back && (!slash || back > slash)) slash = back;
    Buf cue = {0};
    disc_cue(slash ? slash + 1 : bin_path, &cue);
    ok = ok && file_write(bin_path, bin.data, bin.len) && file_write(target, cue.data, cue.len);
    uint64_t size = ok ? bin.len : 0;
    buf_free(&cue);
    buf_free(&bin);
    free(bin_path);
    return size;
}

static int build(Game* g, const char* out, const Options* o, char** target_out) {
    Buf eng = {0}, sfo = {0};
    if (!load_engine(o->engine, &eng)) return 2;
    if (g_target->id == TARGET_VITA && !load_vita_sfo(o->engine, &sfo)) {
        buf_free(&eng);
        return 2;
    }
    Pack pack = {0};
    log_msg("database");
    Node* lang = NULL;
    char* credits = NULL;
    const char* voices = o->voices && g_target->id != TARGET_PS1 && dir_exists(o->voices) ? o->voices : NULL;
    Node* db = build_database(g, &pack, voices, &lang, &credits);
    log_msg("maps");
    log_msg("  %d maps", build_maps(g, node_get(db, "tilesets"), node_get(lang, "lines"), &pack));
    log_msg("images");
    log_msg("  %d images", build_images(g, node_get(db, "actors"), &pack, o->jobs));
    log_msg("audio");
    log_msg("  %d files", g_target->id == TARGET_PS1 ? build_ps1_audio(g, &pack, o->jobs) : build_audio(g, &pack));
    log_msg("fonts");
    build_fonts(g, lang, credits, &pack, o->jobs);
    if (voices) {
        log_msg("voices");
        int clips = build_voices(voices, &pack);
        log_msg("  %d clips", clips);
        if (clips) {
            char* idx = str_fmt("%d\n", clips);
            buf_put(&pack_add(&pack, "vo/index.txt")->data, idx, strlen(idx));
            free(idx);
        }
    }
    node_free(lang);
    free(credits);
    node_free(db);
    int failed = 0;
    for (int i = 0; i < pack.count; i++) {
        if (pack.items[i].error) {
            sl_add(&g_errors, xstrdup(pack.items[i].error));
            failed++;
        }
    }
    char* target = output_path(out);
    char* dir = parent_dir(target);
    make_dirs(dir);
    free(dir);
    Buf file = {0};
    uint64_t size = 0;
    int ok;
    switch (g_target->id) {
        case TARGET_VITA:
            size = package_vita(g, &pack, &eng, &sfo, o, out, &file);
            ok = size > 0;
            break;
        case TARGET_WII:
            size = package_wii(g, &pack, &eng, o, out, &file);
            ok = size > 0;
            break;
        case TARGET_WIIU:
            size = package_wiiu(g, &pack, &eng, o, out, &file);
            ok = size > 0;
            break;
        case TARGET_XENON:
            size = package_xenon(&pack, &eng, o, out, &file);
            ok = size > 0;
            break;
        case TARGET_3DS:
            ok = package_3ds(g, &pack, &eng, o, &file);
            break;
        case TARGET_PS2:
            ok = package_ps2(g, &pack, &eng, o, &file, &failed);
            break;
        case TARGET_PS3:
            size = package_ps3(g, &pack, &eng, o, target);
            ok = size > 0;
            break;
        case TARGET_PS1:
            size = package_ps1(g, &pack, &eng, target, &failed);
            ok = size > 0;
            break;
        default:
            ok = package_psp(g, &pack, &eng, o, &file);
            break;
    }
    pack_free(&pack);
    buf_free(&eng);
    buf_free(&sfo);
    if (ok && g_target->id != TARGET_PS3 && g_target->id != TARGET_PS1) {
        size += file.len;
        ok = file_write(target, file.data, file.len);
    }
    for (int i = 0; i < g_errors.count && i < 50; i++) log_msg("  problem: %s", g_errors.items[i]);
    if (g_errors.count > 50) log_msg("  ... %d more problems", g_errors.count - 50);
    if (!ok) {
        fprintf(stderr, "error: cannot write %s\n", target);
        buf_free(&file);
        free(target);
        return 2;
    }
    log_msg("done (%.1f MB) -> %s", size / 1048576.0, target);
    if (g_target->id == TARGET_3DS && o->cia) {
        char* cia = ends_with_ci(out, ".3dsx") ? cia_path_for(out) : str_fmt("%s/tcoaal.cia", out);
        if (!write_cia(file.data, file.len, cia)) failed++;
        free(cia);
    }
    buf_free(&file);
    *target_out = target;
    return failed ? 1 : 0;
}

static int open_game(Game* g, const char* dir, int any_version) {
    if (!game_open(g, dir)) {
        char* m = str_fmt("Could not find the game in %s (expected a folder containing www/js/main.js).", dir);
        fprintf(stderr, "error: %s\n", m);
        if (ui_is_native()) ui_message(UI_TITLE, m, 1);
        free(m);
        return 0;
    }
    log_msg("game: %s", g->root);
    log_msg("game version %s", g->version);
    if (!is_supported(g->version) && !any_version) {
        char* m = str_fmt("This game version (%s) is not supported. The port needs version 3.0.12.", g->version);
        fprintf(stderr, "error: %s\n", m);
        if (ui_is_native()) ui_message(UI_TITLE, m, 1);
        free(m);
        return 0;
    }
    return 1;
}

static const Target* choose_target(void) {
    const Target* avail[TARGET_COUNT];
    const char* names[TARGET_COUNT];
    int n = 0;
    for (int i = 0; i < TARGET_COUNT; i++) {
        if (!has_engine((TargetId)i)) continue;
        avail[n] = target_get((TargetId)i);
        names[n] = avail[n]->console;
        n++;
    }
    if (n == 0) {
        ui_message(UI_TITLE, "This builder was compiled without any console engine.", 1);
        return NULL;
    }
    if (n == 1) return avail[0];
    int pick = ui_choose(UI_TITLE, "Which console do you want to play The Coffin of Andy and Leyley on?", names, n);
    return pick >= 0 ? avail[pick] : NULL;
}

static const char* out_prompt(void) {
    switch (g_target->id) {
        case TARGET_VITA:
            return "Choose a folder to save it in (TCOAAL.vpk and a data folder will be created inside)";
        case TARGET_WII:
            return "Choose where to save it: your SD card or USB drive, or any folder (an apps folder will be created inside)";
        case TARGET_WIIU:
            return "Choose where to save it: your SD card, or any folder (a wiiu folder will be created inside)";
        case TARGET_XENON:
            return "Choose where to save it: a USB drive formatted as FAT32, or any folder (xenon.elf and a tcoaal folder will be created inside)";
        case TARGET_3DS:
            return "Choose where to save it: your SD card, or any folder (a 3ds folder will be created inside)";
        case TARGET_PS2:
            return "Choose where to save TCOAAL.iso: the DVD folder of your OPL drive, or any folder";
        case TARGET_PS3:
            return "Choose where to save TCOAAL.pkg: the root of a USB drive, or any folder";
        case TARGET_PS1:
            return "Choose where to save the disc image (TCOAAL.cue and TCOAAL.bin)";
        default:
            return "Choose where to save it: your memory stick, or any folder (a PSP folder will be created inside)";
    }
}

static char* done_message(const char* target, const char* out) {
    switch (g_target->id) {
        case TARGET_VITA:
            return str_fmt("Done!\n\nIn\n%s\nyou will find TCOAAL.vpk and a data folder.\n\nCopy the data folder to ux0: on your Vita, so the game data ends up at "
                           "ux0:data/TCOAAL/tcoaal.pak. Then copy TCOAAL.vpk to the Vita and install it with VitaShell.",
                           out);
        case TARGET_WII:
            return str_fmt("Done!\n\nYour game is at:\n%s\n\nIf you picked your SD card or USB drive, start it from the Homebrew Channel. Otherwise copy the apps folder from\n%s\n"
                           "to the root of your SD card or USB drive.",
                           target, out);
        case TARGET_WIIU:
            return str_fmt("Done!\n\nYour game is at:\n%s\n\nIf you picked your SD card, start it from the Wii U Menu with Aroma. Otherwise copy the wiiu folder from\n%s\n"
                           "to the root of your SD card.",
                           target, out);
        case TARGET_XENON:
            return str_fmt("Done!\n\nIn\n%s\nyou will find xenon.elf and a tcoaal folder.\n\nCopy both to the root of a FAT32 USB drive if they are not on one already, plug it "
                           "into an Xbox 360 with RGH or JTAG and boot XeLL. It loads xenon.elf from the drive and the game starts. Saves go to the tcoaal folder on the drive.",
                           out);
        case TARGET_3DS:
            return str_fmt("Done!\n\nYour game is at:\n%s\n\nOpen it in Azahar, or start it from the Homebrew Launcher. If you didn't pick your SD card, copy the 3ds folder from\n%s\nto the root of the SD card.\n\nAlso make tcoaal.cia? It installs the game to the HOME menu with FBI, and it's about as big as tcoaal.3dsx.",
                           target, out);
        case TARGET_PS2:
            return str_fmt("Done!\n\nYour game is at:\n%s\n\nOpen it in PCSX2, or put it in the DVD folder of the USB drive, hard drive or network share you use with Open PS2 Loader and start it from there.",
                           target);
        case TARGET_PS3:
            return str_fmt("Done!\n\nYour game is at:\n%s\n\nCopy TCOAAL.pkg to the root of a USB drive if it is not there already, then on the PS3 open Package Manager, "
                           "Install Package Files and pick it. In RPCS3, use File, Install Packages.",
                           target);
        case TARGET_PS1:
            return str_fmt("Done!\n\nYour disc image is at:\n%s\n\nOpen it in DuckStation or PCSX-Redux, copy it to an optical drive emulator, or burn it to a CD-R.",
                           target);
        default:
            return str_fmt("Done!\n\nYour game is at:\n%s\n\nIf you picked your memory stick, it is ready to play. Otherwise copy the PSP folder from\n%s\nto the root of your memory stick.",
                           target, out);
    }
}

static void offer_cia(const char* target, const char* out, const char* message, int* rc) {
    if (!embedded_3ds_cia_size) {
        ui_message(UI_TITLE, message, 0);
        return;
    }
    if (!ui_yes_no(UI_TITLE, message, "Make tcoaal.cia", "No thanks")) return;
    char* path = str_fmt("%s/tcoaal.cia", out);
    if (make_cia(target, path) == 0) {
        char* m = str_fmt("tcoaal.cia is at:\n%s\n\nCopy it to the SD card, install it with FBI, then delete the .cia file. In Azahar, use File, then Install CIA.", path);
        ui_message(UI_TITLE, m, 0);
        free(m);
    } else {
        ui_message(UI_TITLE, "Making tcoaal.cia failed. Run the builder from a terminal to see the details.", 1);
        *rc = *rc ? *rc : 1;
    }
    free(path);
}

static int run_interactive(Options* o) {
    ui_use_native(ui_native_available());
    set_fatal_hook(fatal_dialog);
    if (!o->target) o->target = choose_target();
    if (!o->target) return 3;
    use_target(o->target);
    log_msg("%s builder", UI_TITLE);
    char* dir = choose_game();
    if (!dir) return 3;
    Game g;
    int ok = open_game(&g, dir, o->any_version);
    free(dir);
    if (!ok) return 2;
    if (!check_copy(&g)) return 3;
    char* out = ui_pick_folder(UI_TITLE, out_prompt());
    if (!out) return 3;
    char* target = NULL;
    int rc = build(&g, out, o, &target);
    if (target) {
        char* m = done_message(target, out);
        if (g_target->id == TARGET_3DS && !o->cia) offer_cia(target, out, m, &rc);
        else ui_message(UI_TITLE, m, 0);
        free(m);
    } else {
        ui_message(UI_TITLE, "The build failed. Run the builder from a terminal to see the details.", 1);
    }
    free(target);
    free(out);
    return rc;
}

int main(int argc, char** argv) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
    int wargc = 0;
    wchar_t** wargv = CommandLineToArgvW(GetCommandLineW(), &wargc);
    if (wargv) {
        argc = wargc;
        argv = xmalloc(sizeof(char*) * (size_t)(argc + 1));
        for (int i = 0; i < argc; i++) argv[i] = wide_to_utf8(wargv[i]);
        argv[argc] = NULL;
        LocalFree(wargv);
    }
#endif
    const char* game_dir = NULL;
    const char* out = NULL;
    const char* update = NULL;
    const char* to_cia = NULL;
    Options o = {NULL, NULL, NULL, NULL, NULL, cpu_count(), 0, 0};
    for (int i = 1; i < argc; i++) {
        const char* a = argv[i];
        if ((!strcmp(a, "-o") || !strcmp(a, "--out")) && i + 1 < argc) out = argv[++i];
        else if ((!strcmp(a, "-t") || !strcmp(a, "--target")) && i + 1 < argc) {
            o.target = target_find(argv[++i]);
            if (!o.target) {
                char names[96];
                target_names(names, sizeof(names));
                fprintf(stderr, "error: unknown target %s (use %s)\n", argv[i], names);
                return 2;
            }
        } else if (!strcmp(a, "--engine") && i + 1 < argc) o.engine = argv[++i];
        else if (!strcmp(a, "--pak") && i + 1 < argc) o.pak = argv[++i];
        else if (!strcmp(a, "--voices") && i + 1 < argc) o.voices = argv[++i];
        else if (!strcmp(a, "--folder") && i + 1 < argc) o.folder = argv[++i];
        else if (!strcmp(a, "--jobs") && i + 1 < argc) o.jobs = atoi(argv[++i]);
        else if (!strcmp(a, "--update-engine") && i + 1 < argc) update = argv[++i];
        else if (!strcmp(a, "--make-cia") && i + 1 < argc) to_cia = argv[++i];
        else if (!strcmp(a, "--allow-any-version")) o.any_version = 1;
        else if (!strcmp(a, "--cia")) o.cia = 1;
        else if (!strcmp(a, "-h") || !strcmp(a, "--help")) {
            usage();
            return 0;
        } else if (a[0] == '-') {
            fprintf(stderr, "error: unknown option %s\n", a);
            usage();
            return 2;
        } else if (!game_dir) game_dir = a;
        else {
            usage();
            return 2;
        }
    }
    if (update) return update_engine(update, o.engine, o.jobs);
    if (to_cia) return make_cia(to_cia, out);
    if (!out) out = "out";
    if (!game_dir) return run_interactive(&o);
    use_target(o.target ? o.target : target_get(TARGET_PSP));
    if (o.pak && g_target->id == TARGET_PS1) {
        fprintf(stderr, "error: --pak is for the desktop test build, which does not run the PS1 data\n");
        return 2;
    }
    Game g;
    if (!open_game(&g, game_dir, o.any_version)) return 2;
    if (!check_copy(&g)) return 3;
    char* target = NULL;
    int rc = build(&g, out, &o, &target);
    free(target);
    sl_free(&g_errors);
    return rc;
}
