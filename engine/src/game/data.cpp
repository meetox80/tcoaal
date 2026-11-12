#include "data.h"

#include <cstdio>
#include <cstring>

#include "../core/fs.h"
#include "../platform/platform.h"

namespace data {

static Document g_doc;
#ifndef PSN00BSDK
static std::unordered_map<std::string, const Value*> g_lines;
static std::unordered_map<std::string, std::string> g_labels;
#endif
static std::shared_ptr<MapData> g_current;
static std::unordered_map<int, std::weak_ptr<MapData>> g_maps;

bool load() {
    if (!g_doc.load_data("game.bin")) {
        plat::log("failed to load game.bin");
        return false;
    }
#ifndef PSN00BSDK
    const Value& lang = g_doc.root()["lang"];
    const Value& lines = lang["lines"];
    for (const Member* m = lines.begin_members(); m != lines.end_members(); ++m) g_lines[m->name()] = &m->v;
    const Value& labels = lang["labels"];
    for (const Member* m = labels.begin_members(); m != labels.end_members(); ++m) g_labels[m->name()] = m->v.string();
#endif
    return true;
}

const Value& root() { return g_doc.root(); }
const Value& system() { return g_doc.root()["system"]; }
const Value& actors() { return g_doc.root()["actors"]; }
const Value& actor(int id) { return actors()[id]; }
const Value& items() { return g_doc.root()["items"]; }
const Value& item(int id) { return items()[id]; }
const Value& common_events() { return g_doc.root()["commonEvents"]; }
const Value& common_event(int id) { return common_events()[id]; }
const Value& map_info(int id) { return g_doc.root()["mapInfos"][id]; }
const Value& plugin(const char* name) { return g_doc.root()["plugins"][name]; }
const Value& lang() { return g_doc.root()["lang"]; }

std::shared_ptr<MapData> load_map(int id) {
    auto it = g_maps.find(id);
    if (it != g_maps.end()) {
        if (auto sp = it->second.lock()) return sp;
    }
    char name[64];
    std::snprintf(name, sizeof(name), "maps/Map%03d.bin", id);
    auto doc = std::make_shared<Document>();
    if (!doc->load_data(name)) {
        plat::log("failed to load map %d", id);
        return nullptr;
    }
    auto m = std::make_shared<MapData>();
    m->id = id;
    m->doc = doc;
    m->root = &doc->root();
    m->width = (*m->root)["width"].as_int();
    m->height = (*m->root)["height"].as_int();
    m->scroll_type = (*m->root)["scrollType"].as_int();
    m->tileset_id = (*m->root)["tilesetId"].as_int();
    m->dash_disabled = (*m->root)["disableDashing"].truthy();
    m->pass = (*m->root)["pass"].blob();
    m->region = (*m->root)["region"].blob();
    m->flags = (*m->root)["flags"].blob();
    g_maps[id] = m;
    return m;
}

int tileset_flag(int tileset, int tile_id) {
    char key[16];
    std::snprintf(key, sizeof(key), "%d", tileset);
    return g_doc.root()["tilesets"][key][tile_id].as_int();
}

std::shared_ptr<MapData> current_map() { return g_current; }
void set_current_map(std::shared_ptr<MapData> m) { g_current = std::move(m); }

#ifdef PSN00BSDK
static const Value* find_sorted(const Value& obj, const char* key) {
    const Member* lo = obj.begin_members();
    const Member* hi = obj.end_members();
    while (lo < hi) {
        const Member* mid = lo + (hi - lo) / 2;
        int c = std::strcmp(mid->name(), key);
        if (!c) return &mid->v;
        if (c < 0) lo = mid + 1;
        else hi = mid;
    }
    return nullptr;
}

const Value* lang_lines(const std::string& id) {
    const char* key = id.c_str();
    if (g_current) {
        if (const Value* v = find_sorted((*g_current->root)["lines"], key)) return v;
    }
    for (auto& kv : g_maps) {
        std::shared_ptr<MapData> m = kv.second.lock();
        if (!m || m == g_current) continue;
        if (const Value* v = find_sorted((*m->root)["lines"], key)) return v;
    }
    return find_sorted(lang()["lines"], key);
}

const Value* lang_label_value(const std::string& id) { return find_sorted(lang()["labels"], id.c_str()); }

std::string lang_label(const std::string& id) {
    const Value* v = lang_label_value(id);
    return v ? v->string() : id;
}
#else
const Value* lang_lines(const std::string& id) {
    auto it = g_lines.find(id);
    return it == g_lines.end() ? nullptr : it->second;
}

const Value* lang_label_value(const std::string& id) { return lang()["labels"].find(id.c_str()); }

std::string lang_label(const std::string& id) {
    auto it = g_labels.find(id);
    return it == g_labels.end() ? id : it->second;
}
#endif

std::string lang_menu(const std::string& key) {
    const Value* v = g_doc.root()["lang"]["menus"].find(key.c_str());
    return v ? v->string() : key;
}

std::string lang_sys(const std::string& key) {
    const Value* v = g_doc.root()["lang"]["sys"].find(key.c_str());
    return v ? v->string() : key;
}

}
