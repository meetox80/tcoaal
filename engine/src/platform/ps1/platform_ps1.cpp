#include "../platform.h"

#include <psxgpu.h>
#include <psxpad.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#include "../../core/fs.h"
#include "card.h"
#include "disc.h"
#include "pad.h"
#include "timebase.h"
#include "video.h"
#include "vram.h"

extern "C" int atexit(void (*)(void)) { return 0; }

constexpr uint32_t SCRIPT_RING = 256;

struct TestHook {
    uint32_t frame;
    uint32_t hold_at;
    uint16_t buttons;
    uint16_t lockstep;
    uint16_t script[SCRIPT_RING];
};

extern "C" volatile TestHook tcoaal_test;
volatile TestHook tcoaal_test;

namespace plat {

namespace {

constexpr int TICK_RATE = 60;
constexpr int MAX_TICKS = 4;

struct Map {
    uint16_t pad;
    uint32_t button;
};

constexpr Map MAP[] = {
    {PAD_UP, BTN_UP},         {PAD_DOWN, BTN_DOWN},       {PAD_LEFT, BTN_LEFT},     {PAD_RIGHT, BTN_RIGHT},
    {PAD_CROSS, BTN_OK},      {PAD_CIRCLE, BTN_CANCEL},   {PAD_TRIANGLE, BTN_MENU}, {PAD_SQUARE, BTN_DASH},
    {PAD_L1, BTN_PAGEUP},     {PAD_L2, BTN_PAGEUP},       {PAD_R1, BTN_PAGEDOWN},   {PAD_R2, BTN_PAGEDOWN},
    {PAD_START, BTN_START},   {PAD_SELECT, BTN_SELECT},
};

Options g_options;
std::string g_data_root;
std::string g_save_root = "bu00:";
bool g_running = true;
uint32_t g_buttons;
uint32_t g_last_field;
uint32_t g_tick_credit;
uint64_t g_frame;
uint32_t g_stats_field;

}

bool init(int, char**) {
    video::init();
    vram::reset();
    pad::init();
    timebase::init();
    if (!disc::init()) {
        log("disc: TCOAAL.PAK not found");
        return false;
    }
    card::init();
    g_last_field = video::fields();
    return true;
}

int pending_ticks() {
    if (tcoaal_test.lockstep) return 1;
    uint32_t now = video::fields(), elapsed = now - g_last_field;
    g_last_field = now;
    g_tick_credit += (elapsed ? elapsed : 1) * TICK_RATE;
    int ticks = static_cast<int>(g_tick_credit / video::rate());
    g_tick_credit -= ticks * video::rate();
    return ticks > MAX_TICKS ? MAX_TICKS : ticks;
}

void shutdown() {}
bool running() { return g_running; }
void request_exit() { g_running = false; }

void poll() {
    pad::poll();
    uint16_t held = pad::held();
    uint32_t b = 0;
    for (const Map& m : MAP) {
        if (held & m.pad) b |= m.button;
    }
    if (tcoaal_test.lockstep) b |= tcoaal_test.script[g_frame % SCRIPT_RING];
    g_buttons = b | tcoaal_test.buttons;
}

uint32_t buttons() { return g_buttons; }
uint64_t frame_count() { return g_frame; }
double time_seconds() { return static_cast<double>(video::fields()) / video::rate(); }

void frame_end() {
    video::flip();
    if (tcoaal_test.lockstep && tcoaal_test.hold_at == g_frame) {
        DrawSync(0);
        while (tcoaal_test.hold_at == g_frame) {
        }
    }
    tcoaal_test.frame = static_cast<uint32_t>(++g_frame);
    if (g_frame % 600) return;
    HeapUsage u;
    GetHeapUsage(&u);
    uint32_t now = video::fields();
    log("stats: 600 frames in %d fields, heap %d KB used, %d KB peak, %d KB free", static_cast<int>(now - g_stats_field), static_cast<int>(u.alloc >> 10),
        static_cast<int>(u.alloc_max >> 10), static_cast<int>((u.total - u.alloc) >> 10));
    g_stats_field = now;
}

const Options& options() { return g_options; }
const std::string& data_root() { return g_data_root; }
const std::string& save_root() { return g_save_root; }
bool make_dir(const std::string&) { return true; }

void log(const char* fmt, ...) {
    char line[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    printf("%s\n", line);
}

size_t heap_used() {
    HeapUsage u;
    GetHeapUsage(&u);
    return u.alloc;
}

size_t heap_limit() {
    HeapUsage u;
    GetHeapUsage(&u);
    return u.total;
}

void io_begin() {}
void io_end() {}

bool consume_resume() { return false; }
bool circle_confirms() { return false; }
static bool g_stick = false;
void set_stick_enabled(bool on) { g_stick = on; }
bool stick_enabled() { return g_stick; }
bool debug_warp(int&, int&, int&, int&) { return false; }
bool start_thread(ThreadFn) { return false; }
Mutex* mutex_create() { return nullptr; }
void mutex_lock(Mutex*) {}
void mutex_unlock(Mutex*) {}

void sleep_ms(int ms) {
    for (int n = (ms * video::rate() + 999) / 1000; n > 0; n--) VSync(0);
}

}

namespace fs {

bool read_data(const std::string& rel, std::vector<uint8_t>& out) {
    const disc::Entry* e = disc::find(rel.c_str());
    if (!e) return false;
    out.resize(disc::sectors(e->size) * disc::SECTOR);
    if (!disc::read(*e, out.data())) return false;
    out.resize(e->size);
    return true;
}

bool data_exists(const std::string& rel) { return disc::find(rel.c_str()) != nullptr; }

namespace {

struct Record {
    std::string name;
    std::vector<uint8_t> data;
};

std::string logical(const std::string& path) {
    const std::string& root = plat::save_root();
    return path.compare(0, root.size(), root) == 0 ? path.substr(root.size()) : path;
}

char slot_of(const std::string& name) {
    if (name == "config.sav" || name == "global.sav") return 'S';
    if (name.compare(0, 4, "auto") == 0) return 'A';
    if (name.compare(0, 4, "file") == 0) {
        int n = static_cast<int>(strtol(name.c_str() + 4, nullptr, 10));
        if (n >= 1 && n <= profile::SAVE_SLOTS) return static_cast<char>('0' + n);
    }
    return 0;
}

const char* title_of(char slot) {
    static char buf[32];
    if (slot == 'S') return "Andy and Leyley System";
    if (slot == 'A') return "Andy and Leyley Autosave";
    snprintf(buf, sizeof(buf), "Andy and Leyley Save %c", slot);
    return buf;
}

uint32_t rd32(const uint8_t* p) { return p[0] | p[1] << 8 | p[2] << 16 | static_cast<uint32_t>(p[3]) << 24; }

void put32(std::vector<uint8_t>& out, uint32_t v) {
    for (int k = 0; k < 4; k++) out.push_back(static_cast<uint8_t>(v >> (k * 8)));
}

std::vector<Record> records(char slot) {
    std::vector<Record> out;
    std::vector<uint8_t> p;
    if (!slot || !card::load(slot, p)) return out;
    size_t at = 0;
    while (at + 5 <= p.size()) {
        size_t nl = p[at];
        if (at + 1 + nl + 4 > p.size()) break;
        Record r;
        r.name.assign(reinterpret_cast<const char*>(&p[at + 1]), nl);
        uint32_t size = rd32(&p[at + 1 + nl]);
        at += 1 + nl + 4;
        if (at + size > p.size()) break;
        r.data.assign(p.begin() + at, p.begin() + at + size);
        at += size;
        out.push_back(std::move(r));
    }
    return out;
}

bool commit(char slot, const std::vector<Record>& rs) {
    if (rs.empty()) return card::erase(slot);
    std::vector<uint8_t> p;
    for (const Record& r : rs) {
        p.push_back(static_cast<uint8_t>(r.name.size()));
        p.insert(p.end(), r.name.begin(), r.name.end());
        put32(p, static_cast<uint32_t>(r.data.size()));
        p.insert(p.end(), r.data.begin(), r.data.end());
    }
    return card::store(slot, title_of(slot), p);
}

}

bool read_file(const std::string& path, std::vector<uint8_t>& out) {
    std::string name = logical(path);
    for (Record& r : records(slot_of(name))) {
        if (r.name != name) continue;
        out = std::move(r.data);
        return true;
    }
    return false;
}

bool write_file(const std::string& path, const void* data, size_t size) {
    std::string name = logical(path);
    char slot = slot_of(name);
    if (!slot) return false;
    std::vector<Record> rs = slot == 'S' ? records(slot) : std::vector<Record>();
    auto bytes = static_cast<const uint8_t*>(data);
    bool found = false;
    for (Record& r : rs) {
        if (r.name != name) continue;
        r.data.assign(bytes, bytes + size);
        found = true;
    }
    if (!found) rs.push_back(Record{name, std::vector<uint8_t>(bytes, bytes + size)});
    return commit(slot, rs);
}

bool exists(const std::string& path) {
    std::string name = logical(path);
    for (const Record& r : records(slot_of(name))) {
        if (r.name == name) return true;
    }
    return false;
}

bool remove_file(const std::string& path) {
    std::string name = logical(path);
    char slot = slot_of(name);
    std::vector<Record> rs = records(slot), kept;
    for (Record& r : rs) {
        if (r.name != name) kept.push_back(std::move(r));
    }
    return kept.size() == rs.size() || commit(slot, kept);
}

std::vector<std::string> list_dir(const std::string&) {
    std::vector<std::string> out;
    char slots[2 + profile::SAVE_SLOTS] = {'S', 'A'};
    for (int n = 1; n <= profile::SAVE_SLOTS; n++) slots[1 + n] = static_cast<char>('0' + n);
    for (char slot : slots) {
        for (const Record& r : records(slot)) out.push_back(r.name);
    }
    return out;
}

std::string save_path(const std::string& rel) { return plat::save_root() + rel; }

}
