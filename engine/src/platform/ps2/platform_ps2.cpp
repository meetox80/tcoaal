#include "../audio_out.h"
#include "../gfx.h"
#include "../platform.h"
#include "../common/testing.h"
#include "../../core/fs.h"
#include "ps2.h"
#include "build_info.h"

#include <audsrv.h>
extern "C" {
#include <debug.h>
}
#include <delaythread.h>
#define NEWLIB_PORT_AWARE
#include <fileXio_rpc.h>
#include <iopcontrol.h>
#include <iopheap.h>
#include <kernel.h>
#include <libcdvd.h>
#include <libpad.h>
#include <loadfile.h>
#include <malloc.h>
#include <sbv_patches.h>
#include <sifrpc.h>
#include <timer.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#define IRX(name)                         \
    extern "C" unsigned char name##_irx[]; \
    extern "C" unsigned int size_##name##_irx;

IRX(iomanX)
IRX(fileXio)
IRX(libsd)
IRX(audsrv)
IRX(usbd)
IRX(bdm)
IRX(bdmfs_fatfs)
IRX(usbmass_bd)
IRX(cdfs)

namespace plat {

static Options g_opt;
static volatile bool g_running = true;
static uint32_t g_buttons = 0;
static uint64_t g_frame = 0;
static FILE* g_log = nullptr;
static bool g_log_stdout = true;
static char g_padbuf[256] __attribute__((aligned(64)));
static bool g_pad_open = false;
static bool g_pad_analog = false;
static uint32_t g_last_vsync = 0;
static int g_tick_acc = 0;
static std::vector<std::string> g_notes;

static const char* const PAK_NAME = "TCOAAL.PAK";
static const char* const DISC_PAK = "cdrom0:\\TCOAAL.PAK;1";
static const char* const CDFS_PAK = "cdfs:/TCOAAL.PAK";

enum class Boot { Disc, Host, Usb, Other };

static void note(const char* fmt, ...) {
    char line[160];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    std::printf("tcoaal: %s\n", line);
    g_notes.push_back(line);
}

static bool load(const char* name, unsigned char* data, unsigned int size) {
    int res = 0;
    int id = SifExecModuleBuffer(data, size, 0, nullptr, &res);
    if (id >= 0 && res != 1) return true;
    note("module %s failed (%d, %d)", name, id, res);
    return false;
}

static void rom(const char* path) {
    int id = SifLoadModule(path, 0, nullptr);
    if (id < 0) note("module %s failed (%d)", path, id);
}

#define LOAD(name) load(#name, name##_irx, size_##name##_irx)

static void start_iop(bool usb) {
    SifInitRpc(0);
    while (!SifIopReset("", 0)) {
    }
    while (!SifIopSync()) {
    }
    SifInitRpc(0);
    SifInitIopHeap();
    SifLoadFileInit();
    sbv_patch_enable_lmb();
    sbv_patch_disable_prefix_check();
    LOAD(iomanX);
    LOAD(fileXio);
    rom("rom0:SIO2MAN");
    rom("rom0:MCMAN");
    rom("rom0:MCSERV");
    rom("rom0:PADMAN");
    LOAD(libsd);
    LOAD(audsrv);
    if (usb) {
        LOAD(usbd);
        LOAD(bdm);
        LOAD(bdmfs_fatfs);
        LOAD(usbmass_bd);
    }
    fileXioInit();
}

static Boot boot_kind(const std::string& path) {
    if (path.rfind("cdrom", 0) == 0) return Boot::Disc;
    if (path.rfind("host", 0) == 0) return Boot::Host;
    if (path.rfind("mass", 0) == 0) return Boot::Usb;
    return Boot::Other;
}

static std::string dir_of(const std::string& path) {
    size_t slash = path.find_last_of("/\\");
    if (slash != std::string::npos) return path.substr(0, slash);
    size_t colon = path.find(':');
    return colon == std::string::npos ? std::string() : path.substr(0, colon + 1);
}

static std::string join(const std::string& dir, const char* name) {
    if (dir.empty() || dir.back() == ':' || dir.back() == '/') return dir + name;
    return dir + "/" + name;
}

static bool usb_mounted() {
    DIR* d = opendir("mass:/");
    if (!d) return false;
    closedir(d);
    return true;
}

static void wait_usb(const std::string& probe) {
    for (int i = 0; i < 100; i++) {
        struct stat st;
        if (probe.empty() ? usb_mounted() : stat(probe.c_str(), &st) == 0) return;
        DelayThread(50 * 1000);
    }
}

static bool try_pack(const std::string& path) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) {
        int err = errno;
        note("%s: %s (%d)", path.c_str(), std::strerror(err), err);
        return false;
    }
    uint8_t h[8] = {};
    bool read = std::fread(h, 1, sizeof(h), f) == sizeof(h);
    std::fclose(f);
    if (!read || std::memcmp(h, "TPAK", 4) != 0) {
        note("%s: not a TCOAAL data pack", path.c_str());
        return false;
    }
    uint32_t version = h[4] | (h[5] << 8) | (h[6] << 16) | ((uint32_t)h[7] << 24);
    if (version != fs::PACK_VERSION) {
        note("%s: pack version %u, this engine reads %u", path.c_str(), (unsigned)version, (unsigned)fs::PACK_VERSION);
        return false;
    }
    fs::init_data(path);
    if (!fs::data_packed()) {
        note("%s: pack index could not be read", path.c_str());
        return false;
    }
    g_opt.data_root = path;
    return true;
}

static bool retry_pack(const std::string& path, int tries) {
    for (int i = 0; i < tries; i++) {
        size_t mark = g_notes.size();
        if (try_pack(path)) return true;
        if (i + 1 == tries) break;
        g_notes.resize(mark);
        DelayThread(250 * 1000);
    }
    return false;
}

static void prepare_disc() {
    if (!sceCdInit(SCECdINoD)) {
        note("cdvd: init failed");
        return;
    }
    int type = sceCdGetDiskType();
    for (int i = 0; i < 100 && type >= SCECdDETCT && type <= SCECdDETCTDVDD; i++) {
        DelayThread(100 * 1000);
        type = sceCdGetDiskType();
    }
    sceCdMmode(type == SCECdPS2DVD ? SCECdMmodeDvd : SCECdMmodeCd);
    for (int i = 0; i < 100 && sceCdDiskReady(1) != SCECdComplete; i++) DelayThread(100 * 1000);
    sceCdlFILE file{};
    bool listed = sceCdSearchFile(&file, "\\TCOAAL.PAK;1") != 0;
    note("disc type 0x%02x, pack %s", type, listed ? "listed on the disc" : "not listed on the disc");
}

static bool scan_usb(const std::string& tried) {
    std::vector<std::string> dirs = {"mass:/"};
    DIR* d = opendir("mass:/");
    if (!d) {
        note("mass:/: no USB drive found");
        return false;
    }
    while (dirent* e = readdir(d)) {
        if (e->d_name[0] != '.') dirs.push_back(std::string("mass:/") + e->d_name);
    }
    closedir(d);
    bool seen = false;
    for (auto& dir : dirs) {
        std::string path = join(dir, PAK_NAME);
        struct stat st;
        if (path == tried || stat(path.c_str(), &st) != 0 || S_ISDIR(st.st_mode)) continue;
        seen = true;
        if (try_pack(path)) return true;
    }
    if (!seen) note("mass:/: no %s in the top folder or one level down", PAK_NAME);
    return false;
}

static bool find_data(Boot kind, const std::string& base) {
    if (!g_opt.data_root.empty()) return try_pack(g_opt.data_root);
    if (kind == Boot::Disc) {
        prepare_disc();
        if (retry_pack(DISC_PAK, 20)) return true;
        return LOAD(cdfs) && retry_pack(CDFS_PAK, 4);
    }
    std::string near = base.empty() ? std::string() : join(base, PAK_NAME);
    if (!near.empty() && try_pack(near)) return true;
    if (kind == Boot::Host) return false;
    if (scan_usb(near)) return true;
    if (kind == Boot::Usb) return false;
    prepare_disc();
    return retry_pack(DISC_PAK, 4);
}

static std::string pick_card() {
    for (const char* card : {"mc0:", "mc1:"}) {
        std::string dir = std::string(card) + "/TCOAAL";
        struct stat st;
        if (stat(dir.c_str(), &st) == 0) return dir;
    }
    for (const char* card : {"mc0:", "mc1:"}) {
        std::string dir = std::string(card) + "/TCOAAL";
        if (mkdir(dir.c_str(), 0777) == 0) return dir;
    }
    return "mc0:/TCOAAL";
}

static void install_icon() {
    for (const char* name : {"icon.sys", "icon.icn"}) {
        std::string dst = g_opt.save_root + "/" + name;
        if (fs::exists(dst)) continue;
        std::vector<uint8_t> data;
        if (fs::read_data(std::string("ps2/") + name, data)) fs::write_file(dst, data.data(), data.size());
    }
}

static void fatal(Boot kind, const std::string& self) {
    init_scr();
    scr_setbgcolor(0x000000);
    scr_setfontcolor(0xFFFFFF);
    scr_clear();
    scr_printf("\n\n  The Coffin of Andy and Leyley (%s)\n\n", TCOAAL_COMMIT);
    scr_printf("  The game data could not be read.\n");
    scr_printf("  Started from: %s\n\n", self.empty() ? "(no path given)" : self.c_str());
    size_t first = g_notes.size() > 14 ? g_notes.size() - 14 : 0;
    for (size_t i = first; i < g_notes.size(); i++) scr_printf("  %s\n", g_notes[i].c_str());
    scr_printf("\n");
    if (kind == Boot::Disc) scr_printf("  Build TCOAAL.iso again with the builder and start it from the new file.\n");
    else scr_printf("  Put TCOAAL.PAK from the builder next to TCOAAL.ELF, from the same build.\n");
    scr_printf("  A photo of this screen helps with a bug report.\n");
    for (;;) SleepThread();
}

bool init(int argc, char** argv) {
    ChangeThreadPriority(GetThreadId(), 64);
    std::string self = argc > 0 && argv[0] ? argv[0] : "";
    Boot kind = boot_kind(self);
    start_iop(kind == Boot::Usb || kind == Boot::Other);
    std::string base = kind == Boot::Disc ? std::string() : dir_of(self);
    if (kind == Boot::Usb) wait_usb(join(base, PAK_NAME));
    if (kind == Boot::Other) wait_usb("");
    g_opt.save_root = pick_card();
    if ((kind == Boot::Host || kind == Boot::Usb) && !base.empty()) {
        g_log = std::fopen(join(base, "log.txt").c_str(), "w");
        g_opt.shot_prefix = join(base, "shot");
        std::vector<std::string> args = testing::read_args_file(join(base, "args.txt"));
        for (auto& a : args) {
            if (!a.empty() && a[0] != '-' && a.find(':') == std::string::npos && a.find('/') == std::string::npos && a.find('.') != std::string::npos) a = join(base, a.c_str());
        }
        testing::parse_args(g_opt, args);
    }
    testing::setup(g_opt);
    bool found = find_data(kind, base);
    log("boot %s, data %s, saves %s", self.c_str(), g_opt.data_root.c_str(), g_opt.save_root.c_str());
    if (!found) fatal(kind, self);
    install_icon();
    padInit(0);
    g_pad_open = padPortOpen(0, 0, g_padbuf) != 0;
    gfx::init();
    g_last_vsync = ps2::vsync_count();
    return true;
}

void shutdown() {
    audio_out::stop();
    gfx::shutdown();
    if (g_log) std::fclose(g_log);
    g_log = nullptr;
    if (g_pad_open) padPortClose(0, 0);
    padEnd();
    Exit(0);
}

bool running() { return g_running; }
void request_exit() { g_running = false; }

static uint32_t stick(int x, int y) {
    if (!stick_enabled()) return 0;
    int dx = x - 128, dy = y - 128;
    if (dx * dx + dy * dy < 64 * 64) return 0;
    uint32_t b = 0;
    if (std::abs(dx) * 5 > std::abs(dy) * 2) b |= dx < 0 ? BTN_LEFT : BTN_RIGHT;
    if (std::abs(dy) * 5 > std::abs(dx) * 2) b |= dy < 0 ? BTN_UP : BTN_DOWN;
    return b;
}

void poll() {
    uint32_t b = 0;
    if (g_pad_open) {
        int state = padGetState(0, 0);
        if (state == PAD_STATE_DISCONN) g_pad_analog = false;
        if (state == PAD_STATE_STABLE && !g_pad_analog) g_pad_analog = padSetMainMode(0, 0, PAD_MMODE_DUALSHOCK, PAD_MMODE_UNLOCK) != 0;
        if (state != PAD_STATE_DISCONN && state != PAD_STATE_ERROR) {
            padButtonStatus st;
            if (padRead(0, 0, &st) != 0) {
                uint32_t p = 0xFFFFu ^ st.btns;
                if (p & PAD_UP) b |= BTN_UP;
                if (p & PAD_DOWN) b |= BTN_DOWN;
                if (p & PAD_LEFT) b |= BTN_LEFT;
                if (p & PAD_RIGHT) b |= BTN_RIGHT;
                if (!(b & (BTN_UP | BTN_DOWN | BTN_LEFT | BTN_RIGHT)) && st.mode >> 4 == 0x7) b |= stick(st.ljoy_h, st.ljoy_v);
                if (p & PAD_CROSS) b |= BTN_OK;
                if (p & PAD_CIRCLE) b |= BTN_CANCEL;
                if (p & PAD_TRIANGLE) b |= BTN_MENU;
                if (p & PAD_SQUARE) b |= BTN_DASH;
                if (p & (PAD_L1 | PAD_L2)) b |= BTN_PAGEUP;
                if (p & (PAD_R1 | PAD_R2)) b |= BTN_PAGEDOWN;
                if (p & PAD_START) b |= BTN_START;
                if (p & PAD_SELECT) b |= BTN_SELECT;
            }
        }
    }
    b |= testing::buttons(g_frame);
    g_buttons = b;
}

uint32_t buttons() { return g_buttons; }
uint64_t frame_count() { return g_frame; }

double time_seconds() { return (double)GetTimerSystemTime() / (double)kBUSCLK; }

static bool scripted() { return g_opt.max_frames > 0 || !g_opt.input_script.empty() || !g_opt.shot_frames.empty(); }

int pending_ticks() {
    uint32_t now = ps2::vsync_count();
    uint32_t dv = now - g_last_vsync;
    g_last_vsync = now;
    if (scripted()) return 1;
    int hz = ps2::refresh_hz();
    g_tick_acc += (int)std::min<uint32_t>(dv, 8) * 60;
    int ticks = g_tick_acc / hz;
    g_tick_acc %= hz;
    return std::clamp(ticks, 1, 4);
}

void frame_end() {
    gfx::end_frame();
    g_frame++;
    if (g_opt.max_frames > 0 && g_frame >= (uint64_t)g_opt.max_frames) g_running = false;
    if (testing::exit_due(g_frame)) g_running = false;
}

const Options& options() { return g_opt; }
const std::string& data_root() { return g_opt.data_root; }
const std::string& save_root() { return g_opt.save_root; }

bool make_dir(const std::string& path) {
    struct stat st;
    if (stat(path.c_str(), &st) == 0) return true;
    return mkdir(path.c_str(), 0777) == 0;
}

void log(const char* fmt, ...) {
    va_list ap;
    if (g_log) {
        va_start(ap, fmt);
        std::vfprintf(g_log, fmt, ap);
        va_end(ap);
        std::fputc('\n', g_log);
        std::fflush(g_log);
    }
    if (g_log_stdout) {
        va_start(ap, fmt);
        std::vprintf(fmt, ap);
        va_end(ap);
        std::putchar('\n');
    }
}

bool debug_warp(int& map, int& x, int& y, int& d) { return testing::warp_due(g_frame, map, x, y, d); }

bool consume_resume() { return false; }

bool circle_confirms() { return false; }

struct Mutex {
    int sema;
};

struct ThreadStart {
    ThreadFn fn;
};

static void thread_entry(void* arg) {
    ThreadFn fn = ((ThreadStart*)arg)->fn;
    delete (ThreadStart*)arg;
    if (fn) fn();
    ExitDeleteThread();
}

int spawn(void (*entry)(void*), void* arg, int stack_size, int priority) {
    ee_thread_t t{};
    t.func = (void*)entry;
    t.stack = memalign(64, (size_t)stack_size);
    t.stack_size = stack_size;
    t.gp_reg = &_gp;
    t.initial_priority = priority;
    int id = CreateThread(&t);
    if (id < 0) return -1;
    if (StartThread(id, arg) < 0) return -1;
    return id;
}

bool start_thread(ThreadFn fn) { return spawn(thread_entry, new ThreadStart{fn}, 0x10000, 80) >= 0; }

Mutex* mutex_create() {
    ee_sema_t s{};
    s.init_count = 1;
    s.max_count = 1;
    Mutex* m = new Mutex();
    m->sema = CreateSema(&s);
    return m;
}

void mutex_lock(Mutex* m) { WaitSema(m->sema); }
void mutex_unlock(Mutex* m) { SignalSema(m->sema); }
void sleep_ms(int ms) { DelayThread(std::max(ms, 1) * 1000); }

size_t heap_used() {
    struct mallinfo mi = mallinfo();
    return (size_t)mi.uordblks;
}

size_t heap_limit() { return 0; }

void io_begin() {}
void io_end() {}

}

namespace audio_out {

static MixFn g_fn = nullptr;
static int g_lock = -1;
static volatile bool g_run = false;
static volatile bool g_done = false;
static int16_t __attribute__((aligned(64))) g_buf[BLOCK * 2];

static void audio_thread(void*) {
    while (g_run) {
        audsrv_wait_audio(sizeof(g_buf));
        if (!g_run) break;
        lock();
        if (g_fn) g_fn(g_buf, BLOCK);
        else std::memset(g_buf, 0, sizeof(g_buf));
        unlock();
        audsrv_play_audio((const char*)g_buf, sizeof(g_buf));
    }
    g_done = true;
    ExitDeleteThread();
}

bool start(MixFn fn) {
    g_fn = fn;
    ee_sema_t s{};
    s.init_count = 1;
    s.max_count = 1;
    g_lock = CreateSema(&s);
    if (audsrv_init() != 0) {
        plat::log("audsrv: %s", audsrv_get_error_string());
        return false;
    }
    audsrv_fmt_t fmt{};
    fmt.bits = 16;
    fmt.freq = RATE;
    fmt.channels = 2;
    if (audsrv_set_format(&fmt) != 0) return false;
    audsrv_set_volume(MAX_VOLUME);
    g_run = true;
    g_done = false;
    if (plat::spawn(audio_thread, nullptr, 0x10000, 32) < 0) {
        g_run = false;
        return false;
    }
    return true;
}

void stop() {
    if (!g_run) return;
    g_run = false;
    for (int i = 0; i < 200 && !g_done; i++) DelayThread(1000);
    audsrv_stop_audio();
    audsrv_quit();
}

void lock() {
    if (g_lock >= 0) WaitSema(g_lock);
}

void unlock() {
    if (g_lock >= 0) SignalSema(g_lock);
}

}
