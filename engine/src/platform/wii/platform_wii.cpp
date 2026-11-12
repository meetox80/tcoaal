#include "../audio_out.h"
#include "../gfx.h"
#include "../platform.h"
#include "../common/testing.h"
#include "../../core/fs.h"

#include <asndlib.h>
#include <fat.h>
#include <gccore.h>
#include <malloc.h>
#include <ogc/lwp_watchdog.h>
#include <sys/stat.h>
#include <unistd.h>
#include <wiiuse/wpad.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace plat {

static Options g_opt;
static volatile bool g_running = true;
static volatile bool g_power_off = false;
static uint32_t g_buttons = 0;
static uint64_t g_frame = 0;
static FILE* g_log = nullptr;
static u64 g_start = 0;
static int64_t g_ticks_done = 0;

constexpr uint32_t DIRS = BTN_UP | BTN_DOWN | BTN_LEFT | BTN_RIGHT;

static void power_pressed() {
    g_power_off = true;
    g_running = false;
}

static void reset_pressed(u32, void*) { g_running = false; }

static std::string base_dir(int argc, char** argv) {
    if (argc > 0 && argv && argv[0]) {
        std::string a = argv[0];
        size_t slash = a.find_last_of('/');
        if (slash != std::string::npos && a.find(':') != std::string::npos) return a.substr(0, slash);
    }
    return "sd:/apps/tcoaal";
}

static uint64_t time_us() { return ticks_to_microsecs(diff_ticks(g_start, gettime())); }

bool init(int argc, char** argv) {
    g_start = gettime();
    SYS_SetPowerCallback(power_pressed);
    SYS_SetResetCallback(reset_pressed);
    fatInitDefault();
    PAD_Init();
    WPAD_Init();
    std::string base = base_dir(argc, argv);
    g_opt.data_root = base + "/tcoaal.pak";
    g_opt.save_root = base + "/saves";
    make_dir(g_opt.save_root);
    g_log = std::fopen((base + "/log.txt").c_str(), "w");
    g_opt.shot_prefix = base + "/shot";
    std::vector<std::string> args = testing::read_args_file(base + "/args.txt");
    for (auto& a : args) {
        if (!a.empty() && a[0] != '-' && a.find(':') == std::string::npos && a.find('/') == std::string::npos && a.find('.') != std::string::npos) a = base + "/" + a;
    }
    testing::parse_args(g_opt, args);
    testing::setup(g_opt);
    fs::init_data(g_opt.data_root);
    gfx::init();
    return true;
}

void shutdown() {
    audio_out::stop();
    gfx::shutdown();
    WPAD_Shutdown();
    if (g_log) std::fclose(g_log);
    g_log = nullptr;
    if (g_power_off) SYS_ResetSystem(SYS_POWEROFF, 0, 0);
}

bool running() { return g_running; }
void request_exit() { g_running = false; }

static uint32_t stick(float x, float y) {
    if (!stick_enabled()) return 0;
    if (x * x + y * y < 0.25f) return 0;
    uint32_t b = 0;
    if (std::fabs(x) * 5 > std::fabs(y) * 2) b |= x < 0 ? BTN_LEFT : BTN_RIGHT;
    if (std::fabs(y) * 5 > std::fabs(x) * 2) b |= y > 0 ? BTN_UP : BTN_DOWN;
    return b;
}

static uint32_t classic_stick(const joystick_t& js) {
    if (js.mag < 0.5f) return 0;
    float rad = js.ang * 3.14159265f / 180.0f;
    return stick(std::sin(rad), std::cos(rad));
}

static uint32_t gamecube(int i) {
    u16 p = PAD_ButtonsHeld(i);
    uint32_t b = 0;
    if (p & PAD_BUTTON_UP) b |= BTN_UP;
    if (p & PAD_BUTTON_DOWN) b |= BTN_DOWN;
    if (p & PAD_BUTTON_LEFT) b |= BTN_LEFT;
    if (p & PAD_BUTTON_RIGHT) b |= BTN_RIGHT;
    if (p & PAD_BUTTON_A) b |= BTN_OK;
    if (p & PAD_BUTTON_B) b |= BTN_CANCEL;
    if (p & PAD_BUTTON_X) b |= BTN_MENU;
    if (p & PAD_BUTTON_Y) b |= BTN_DASH;
    if (p & PAD_TRIGGER_L) b |= BTN_PAGEUP;
    if (p & (PAD_TRIGGER_R | PAD_TRIGGER_Z)) b |= BTN_PAGEDOWN;
    if (p & PAD_BUTTON_START) b |= BTN_START;
    if (!(b & DIRS)) b |= stick(PAD_StickX(i) / 80.0f, PAD_StickY(i) / 80.0f);
    return b;
}

static uint32_t classic(u32 h, const classic_ctrl_t& cc) {
    uint32_t b = 0;
    if (h & WPAD_CLASSIC_BUTTON_UP) b |= BTN_UP;
    if (h & WPAD_CLASSIC_BUTTON_DOWN) b |= BTN_DOWN;
    if (h & WPAD_CLASSIC_BUTTON_LEFT) b |= BTN_LEFT;
    if (h & WPAD_CLASSIC_BUTTON_RIGHT) b |= BTN_RIGHT;
    if (h & WPAD_CLASSIC_BUTTON_A) b |= BTN_OK;
    if (h & WPAD_CLASSIC_BUTTON_B) b |= BTN_CANCEL;
    if (h & WPAD_CLASSIC_BUTTON_X) b |= BTN_MENU;
    if (h & WPAD_CLASSIC_BUTTON_Y) b |= BTN_DASH;
    if (h & (WPAD_CLASSIC_BUTTON_FULL_L | WPAD_CLASSIC_BUTTON_ZL)) b |= BTN_PAGEUP;
    if (h & (WPAD_CLASSIC_BUTTON_FULL_R | WPAD_CLASSIC_BUTTON_ZR)) b |= BTN_PAGEDOWN;
    if (h & WPAD_CLASSIC_BUTTON_PLUS) b |= BTN_START;
    if (h & WPAD_CLASSIC_BUTTON_MINUS) b |= BTN_SELECT;
    if (!(b & DIRS)) b |= classic_stick(cc.ljs);
    return b;
}

static uint32_t wiimote(int i) {
    WPADData* w = WPAD_Data(i);
    if (!w || w->err != WPAD_ERR_NONE) return 0;
    u32 h = w->btns_h;
    if (h & (WPAD_BUTTON_HOME | WPAD_CLASSIC_BUTTON_HOME)) g_running = false;
    if (w->exp.type == WPAD_EXP_CLASSIC) return classic(h, w->exp.classic);
    uint32_t b = 0;
    if (h & WPAD_BUTTON_RIGHT) b |= BTN_UP;
    if (h & WPAD_BUTTON_LEFT) b |= BTN_DOWN;
    if (h & WPAD_BUTTON_UP) b |= BTN_LEFT;
    if (h & WPAD_BUTTON_DOWN) b |= BTN_RIGHT;
    if (h & WPAD_BUTTON_2) b |= BTN_OK;
    if (h & WPAD_BUTTON_1) b |= BTN_CANCEL;
    if (h & WPAD_BUTTON_A) b |= BTN_PAGEDOWN;
    if (h & WPAD_BUTTON_B) b |= BTN_DASH;
    if (h & WPAD_BUTTON_PLUS) b |= BTN_START;
    if (h & WPAD_BUTTON_MINUS) b |= BTN_PAGEUP;
    if (w->exp.type == WPAD_EXP_NUNCHUK && !(b & DIRS)) b |= classic_stick(w->exp.nunchuk.js);
    return b;
}

void poll() {
    PAD_ScanPads();
    WPAD_ScanPads();
    uint32_t b = 0;
    for (int i = 0; i < 4; i++) b |= gamecube(i) | wiimote(i);
    b |= testing::buttons(g_frame);
    g_buttons = b;
}

uint32_t buttons() { return g_buttons; }
uint64_t frame_count() { return g_frame; }

double time_seconds() { return (double)time_us() / 1000000.0; }

static bool scripted() { return g_opt.max_frames > 0 || !g_opt.input_script.empty() || !g_opt.shot_frames.empty(); }

int pending_ticks() {
    if (scripted()) return 1;
    int64_t target = (int64_t)(time_us() * 60 / 1000000);
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
    fsync(fileno(g_log));
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

bool circle_confirms() { return false; }

struct Mutex {
    mutex_t m;
};

struct ThreadStart {
    ThreadFn fn;
};

static void* thread_entry(void* arg) {
    ThreadStart* s = (ThreadStart*)arg;
    ThreadFn fn = s->fn;
    delete s;
    if (fn) fn();
    return nullptr;
}

bool start_thread(ThreadFn fn) {
    lwp_t th;
    ThreadStart* s = new ThreadStart{fn};
    if (LWP_CreateThread(&th, thread_entry, s, nullptr, 256 * 1024, 48) < 0) {
        delete s;
        return false;
    }
    return true;
}

Mutex* mutex_create() {
    Mutex* m = new Mutex();
    LWP_MutexInit(&m->m, false);
    return m;
}

void mutex_lock(Mutex* m) { LWP_MutexLock(m->m); }
void mutex_unlock(Mutex* m) { LWP_MutexUnlock(m->m); }
void sleep_ms(int ms) { usleep((useconds_t)std::max(ms, 1) * 1000); }

}

namespace audio_out {

constexpr int BUFFERS = 3;

static MixFn g_fn = nullptr;
static lwp_t g_thread = LWP_THREAD_NULL;
static mutex_t g_lock = LWP_MUTEX_NULL;
static volatile bool g_run = false;
static int16_t g_buf[BUFFERS][BLOCK * 2] __attribute__((aligned(32)));

static void mix(int i) {
    lock();
    if (g_fn) g_fn(g_buf[i], BLOCK);
    else std::memset(g_buf[i], 0, sizeof(g_buf[i]));
    unlock();
    DCFlushRange(g_buf[i], sizeof(g_buf[i]));
}

static void* audio_thread(void*) {
    int next = 0;
    mix(next);
    ASND_SetVoice(0, VOICE_STEREO_16BIT, RATE, 0, g_buf[next], sizeof(g_buf[next]), MAX_VOLUME, MAX_VOLUME, nullptr);
    next = (next + 1) % BUFFERS;
    mix(next);
    while (g_run) {
        if (ASND_StatusVoice(0) == SND_UNUSED) {
            ASND_SetVoice(0, VOICE_STEREO_16BIT, RATE, 0, g_buf[next], sizeof(g_buf[next]), MAX_VOLUME, MAX_VOLUME, nullptr);
            next = (next + 1) % BUFFERS;
            mix(next);
        } else if (ASND_TestVoiceBufferReady(0) == 1) {
            ASND_AddVoice(0, g_buf[next], sizeof(g_buf[next]));
            next = (next + 1) % BUFFERS;
            mix(next);
        } else {
            usleep(1000);
        }
    }
    return nullptr;
}

bool start(MixFn fn) {
    g_fn = fn;
    LWP_MutexInit(&g_lock, false);
    ASND_Init();
    ASND_Pause(0);
    g_run = true;
    if (LWP_CreateThread(&g_thread, audio_thread, nullptr, nullptr, 64 * 1024, 80) < 0) {
        g_run = false;
        ASND_End();
        return false;
    }
    return true;
}

void stop() {
    if (!g_run) return;
    g_run = false;
    LWP_JoinThread(g_thread, nullptr);
    ASND_StopVoice(0);
    ASND_End();
}

void lock() {
    if (g_lock != LWP_MUTEX_NULL) LWP_MutexLock(g_lock);
}

void unlock() {
    if (g_lock != LWP_MUTEX_NULL) LWP_MutexUnlock(g_lock);
}

}
