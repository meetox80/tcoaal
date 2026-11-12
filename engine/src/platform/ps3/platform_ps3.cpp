#include "../audio_out.h"
#include "../gfx.h"
#include "../platform.h"
#include "../common/testing.h"
#include "../../core/fs.h"

#include <audio/audio.h>
#include <io/pad.h>
#include <lv2/mutex.h>
#include <lv2/systime.h>
#include <malloc.h>
#include <sys/event_queue.h>
#include <sys/process.h>
#include <sys/stat.h>
#include <sys/thread.h>
#include <sysutil/sysutil.h>
#include <unistd.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

SYS_PROCESS_PARAM(1001, 0x100000);

namespace plat {

static Options g_opt;
static volatile bool g_running = true;
static uint32_t g_buttons = 0;
static uint64_t g_frame = 0;
static FILE* g_log = nullptr;
static uint32_t g_pads[MAX_PORT_NUM];
static int64_t g_start_us = 0;
static int64_t g_ticks_done = 0;

static void sysutil_callback(u64 status, u64, void*) {
    if (status == SYSUTIL_EXIT_GAME) g_running = false;
}

static std::string base_dir(int argc, char** argv) {
    std::string base = "/dev_hdd0/game/TCOAAL001/USRDIR";
    if (argc > 0 && argv[0]) {
        std::string a = argv[0];
        size_t slash = a.find_last_of('/');
        if (slash != std::string::npos && a.compare(0, 5, "/dev_") == 0) base = a.substr(0, slash);
    }
    return base;
}

bool init(int argc, char** argv) {
    sysUtilRegisterCallback(SYSUTIL_EVENT_SLOT0, sysutil_callback, nullptr);
    std::string base = base_dir(argc, argv);
    g_opt.data_root = base + "/TCOAAL.PAK";
    g_opt.save_root = base + "/saves";
    make_dir(g_opt.save_root);
    g_log = std::fopen((base + "/log.txt").c_str(), "w");
    g_opt.shot_prefix = base + "/shot";
    std::vector<std::string> args = testing::read_args_file(base + "/args.txt");
    for (auto& a : args) {
        if (!a.empty() && a[0] != '-' && a[0] != '/' && a.find('.') != std::string::npos) a = base + "/" + a;
    }
    testing::parse_args(g_opt, args);
    testing::setup(g_opt);
    fs::init_data(g_opt.data_root);
    ioPadInit(MAX_PORT_NUM);
    gfx::init();
    g_start_us = sysGetSystemTime();
    return true;
}

void shutdown() {
    audio_out::stop();
    gfx::shutdown();
    ioPadEnd();
    sysUtilUnregisterCallback(SYSUTIL_EVENT_SLOT0);
    if (g_log) std::fclose(g_log);
    g_log = nullptr;
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

static uint32_t map_pad(const padData& d) {
    uint32_t b = 0;
    if (d.BTN_UP) b |= BTN_UP;
    if (d.BTN_DOWN) b |= BTN_DOWN;
    if (d.BTN_LEFT) b |= BTN_LEFT;
    if (d.BTN_RIGHT) b |= BTN_RIGHT;
    if (!b) b |= stick((int)d.ANA_L_H, (int)d.ANA_L_V);
    if (d.BTN_CROSS) b |= BTN_OK;
    if (d.BTN_CIRCLE) b |= BTN_CANCEL;
    if (d.BTN_TRIANGLE) b |= BTN_MENU;
    if (d.BTN_SQUARE) b |= BTN_DASH;
    if (d.BTN_L1 || d.BTN_L2) b |= BTN_PAGEUP;
    if (d.BTN_R1 || d.BTN_R2) b |= BTN_PAGEDOWN;
    if (d.BTN_START) b |= BTN_START;
    if (d.BTN_SELECT) b |= BTN_SELECT;
    return b;
}

void poll() {
    sysUtilCheckCallback();
    padInfo info;
    uint32_t b = 0;
    if (ioPadGetInfo(&info) == 0) {
        for (int i = 0; i < MAX_PORT_NUM; i++) {
            if (!info.status[i]) {
                g_pads[i] = 0;
                continue;
            }
            padData d;
            std::memset(&d, 0, sizeof(d));
            if (ioPadGetData(i, &d) == 0 && d.len > 0) g_pads[i] = map_pad(d);
            b |= g_pads[i];
        }
    }
    b |= testing::buttons(g_frame);
    g_buttons = b;
}

uint32_t buttons() { return g_buttons; }
uint64_t frame_count() { return g_frame; }

double time_seconds() { return (double)sysGetSystemTime() / 1000000.0; }

static bool scripted() { return g_opt.max_frames > 0 || !g_opt.input_script.empty() || !g_opt.shot_frames.empty(); }

int pending_ticks() {
    if (scripted()) return 1;
    int64_t target = (sysGetSystemTime() - g_start_us) * 60 / 1000000;
    int64_t owed = target - g_ticks_done;
    if (owed > 4) {
        g_ticks_done = target - 4;
        owed = 4;
    } else if (owed < -2) {
        g_ticks_done = target - 1;
        owed = 1;
    }
    int n = (int)std::clamp<int64_t>(owed, 1, 4);
    g_ticks_done += n;
    return n;
}

void frame_end() {
    gfx::end_frame();
    if (testing::shot_due(g_frame)) testing::write_bmp(testing::shot_path(g_opt, g_frame, "bmp"), gfx::framebuffer(), gfx::SCREEN_W, gfx::SCREEN_H, gfx::SCREEN_W);
    g_frame++;
    if (g_opt.max_frames > 0 && g_frame >= (uint64_t)g_opt.max_frames) g_running = false;
    if (testing::exit_due(g_frame)) g_running = false;
}

const Options& options() { return g_opt; }
const std::string& data_root() { return g_opt.data_root; }
const std::string& save_root() { return g_opt.save_root; }

bool make_dir(const std::string& path) { return mkdir(path.c_str(), 0777) == 0; }

void log(const char* fmt, ...) {
    if (!g_log) return;
    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(g_log, fmt, ap);
    va_end(ap);
    std::fputc('\n', g_log);
    std::fflush(g_log);
}

size_t heap_used() {
    struct mallinfo mi = mallinfo();
    return (size_t)mi.uordblks;
}

size_t heap_limit() { return 0; }

void io_begin() {}
void io_end() {}

bool consume_resume() { return false; }

bool debug_warp(int& map, int& x, int& y, int& d) { return testing::warp_due(g_frame, map, x, y, d); }

bool circle_confirms() {
    s32 v = 1;
    return sysUtilGetSystemParamInt(SYSUTIL_SYSTEMPARAM_ID_ENTER_BUTTON_ASSIGN, &v) == 0 && v == 0;
}

struct Mutex {
    sys_lwmutex_t m;
};

static void thread_entry(void* arg) {
    ThreadFn fn = (ThreadFn)arg;
    if (fn) fn();
    sysThreadExit(0);
}

bool start_thread(ThreadFn fn) {
    sys_ppu_thread_t id;
    char name[] = "worker";
    return sysThreadCreate(&id, thread_entry, (void*)fn, 1100, 0x40000, 0, name) == 0;
}

Mutex* mutex_create() {
    Mutex* m = new Mutex();
    sys_lwmutex_attr_t attr;
    std::memset(&attr, 0, sizeof(attr));
    attr.attr_protocol = SYS_LWMUTEX_PROTOCOL_PRIO;
    attr.attr_recursive = SYS_LWMUTEX_ATTR_NOT_RECURSIVE;
    std::strcpy(attr.name, "mutex");
    sysLwMutexCreate(&m->m, &attr);
    return m;
}

void mutex_lock(Mutex* m) { sysLwMutexLock(&m->m, 0); }
void mutex_unlock(Mutex* m) { sysLwMutexUnlock(&m->m); }
void sleep_ms(int ms) { usleep((useconds_t)ms * 1000); }

}

namespace audio_out {

static MixFn g_fn = nullptr;
static volatile bool g_run = false;
static bool g_open = false;
static u32 g_port = 0;
static audioPortConfig g_cfg;
static sys_event_queue_t g_queue;
static sys_ipc_key_t g_key;
static sys_ppu_thread_t g_thread;
static sys_lwmutex_t g_lock;
static bool g_lock_ok = false;
static uint32_t g_next = 0;

constexpr uint32_t BLOCKS = AUDIO_BLOCK_16;
constexpr uint32_t LEAD = 6;

static void fill(uint32_t block) {
    static int16_t pcm[AUDIO_BLOCK_SAMPLES * 2];
    lock();
    if (g_fn) g_fn(pcm, AUDIO_BLOCK_SAMPLES);
    else std::memset(pcm, 0, sizeof(pcm));
    unlock();
    float* dst = (float*)(uintptr_t)g_cfg.audioDataStart + (size_t)block * 2 * AUDIO_BLOCK_SAMPLES;
    for (int i = 0; i < AUDIO_BLOCK_SAMPLES * 2; i++) dst[i] = pcm[i] * (1.0f / 32768.0f);
}

static void thread_main(void*) {
    while (g_run) {
        sys_event_t ev;
        sysEventQueueReceive(g_queue, &ev, 20 * 1000);
        uint32_t read = (uint32_t)(*(volatile uint64_t*)(uintptr_t)g_cfg.readIndex % BLOCKS);
        uint32_t d = (g_next + BLOCKS - read) % BLOCKS;
        if (d == 0 || d > LEAD + 1) {
            g_next = (read + 1) % BLOCKS;
            d = 1;
        }
        while (d <= LEAD && g_run) {
            fill(g_next);
            g_next = (g_next + 1) % BLOCKS;
            d++;
        }
    }
    sysThreadExit(0);
}

bool start(MixFn fn) {
    g_fn = fn;
    sys_lwmutex_attr_t attr;
    std::memset(&attr, 0, sizeof(attr));
    attr.attr_protocol = SYS_LWMUTEX_PROTOCOL_PRIO;
    attr.attr_recursive = SYS_LWMUTEX_ATTR_NOT_RECURSIVE;
    std::strcpy(attr.name, "audio");
    g_lock_ok = sysLwMutexCreate(&g_lock, &attr) == 0;
    if (audioInit() != 0) return false;
    audioPortParam params;
    std::memset(&params, 0, sizeof(params));
    params.numChannels = AUDIO_PORT_2CH;
    params.numBlocks = BLOCKS;
    params.attrib = AUDIO_PORT_INITLEVEL;
    params.level = 1.0f;
    if (audioPortOpen(&params, &g_port) != 0) {
        audioQuit();
        return false;
    }
    audioGetPortConfig(g_port, &g_cfg);
    audioCreateNotifyEventQueue(&g_queue, &g_key);
    audioSetNotifyEventQueue(g_key);
    sysEventQueueDrain(g_queue);
    std::memset((void*)(uintptr_t)g_cfg.audioDataStart, 0, (size_t)BLOCKS * 2 * AUDIO_BLOCK_SAMPLES * sizeof(float));
    g_next = 1;
    g_run = true;
    g_open = true;
    audioPortStart(g_port);
    char name[] = "audio";
    if (sysThreadCreate(&g_thread, thread_main, nullptr, 400, 0x20000, THREAD_JOINABLE, name) != 0) {
        g_run = false;
        return false;
    }
    return true;
}

void stop() {
    if (!g_open) return;
    if (g_run) {
        g_run = false;
        u64 ret;
        sysThreadJoin(g_thread, &ret);
    }
    audioPortStop(g_port);
    audioRemoveNotifyEventQueue(g_key);
    audioPortClose(g_port);
    sysEventQueueDestroy(g_queue, 0);
    audioQuit();
    g_open = false;
}

void lock() {
    if (g_lock_ok) sysLwMutexLock(&g_lock, 0);
}

void unlock() {
    if (g_lock_ok) sysLwMutexUnlock(&g_lock);
}

}
