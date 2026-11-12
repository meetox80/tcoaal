#include "../audio_out.h"
#include "../gfx.h"
#include "../platform.h"
#include "../common/testing.h"
#include "../../core/fs.h"

#include <malloc.h>
#include <psp2/apputil.h>
#include <psp2/audioout.h>
#include <psp2/ctrl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/power.h>
#include <psp2/system_param.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

extern "C" {
unsigned int _newlib_heap_size_user = 192u << 20;
unsigned int sceUserMainThreadStackSize = 1u << 20;
}

namespace plat {

static const char* BASE = "ux0:data/TCOAAL";

static Options g_opt;
static volatile bool g_running = true;
static uint32_t g_buttons = 0;
static uint64_t g_frame = 0;
static FILE* g_log = nullptr;
static volatile bool g_resumed = false;
static int64_t g_start_us = 0;
static int64_t g_ticks_done = 0;

static int power_callback(int, int, int info, void*) {
    if (info & SCE_POWER_CB_SYSTEM_RESUME) g_resumed = true;
    return 0;
}

static int callback_thread(SceSize, void*) {
    SceUID cb = sceKernelCreateCallback("power", 0, power_callback, nullptr);
    scePowerRegisterCallback(cb);
    while (true) sceKernelDelayThreadCB(1000000);
    return 0;
}

static int64_t now_us() { return (int64_t)sceKernelGetProcessTimeWide(); }

bool init(int, char**) {
    SceUID th = sceKernelCreateThread("callbacks", callback_thread, 0x10000100, 0x4000, 0, 0, nullptr);
    if (th >= 0) sceKernelStartThread(th, 0, nullptr);
    scePowerSetArmClockFrequency(444);
    scePowerSetBusClockFrequency(222);
    scePowerSetGpuClockFrequency(222);
    scePowerSetGpuXbarClockFrequency(166);
    std::string base = BASE;
    make_dir(base);
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
    SceAppUtilInitParam init_param{};
    SceAppUtilBootParam boot_param{};
    sceAppUtilInit(&init_param, &boot_param);
    sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG);
    sceCtrlSetSamplingModeExt(SCE_CTRL_MODE_ANALOG);
    gfx::init();
    g_start_us = now_us();
    return true;
}

void shutdown() {
    audio_out::stop();
    gfx::shutdown();
    if (g_log) std::fclose(g_log);
    g_log = nullptr;
    sceKernelExitProcess(0);
}

bool running() { return g_running; }
void request_exit() { g_running = false; }

static uint32_t stick(uint8_t x, uint8_t y) {
    if (!stick_enabled()) return 0;
    int dx = (int)x - 128, dy = (int)y - 128;
    if (dx * dx + dy * dy < 64 * 64) return 0;
    uint32_t b = 0;
    if (std::abs(dx) * 5 > std::abs(dy) * 2) b |= dx < 0 ? BTN_LEFT : BTN_RIGHT;
    if (std::abs(dy) * 5 > std::abs(dx) * 2) b |= dy < 0 ? BTN_UP : BTN_DOWN;
    return b;
}

void poll() {
    SceCtrlData pad{};
    sceCtrlPeekBufferPositiveExt2(0, &pad, 1);
    uint32_t b = 0;
    if (pad.buttons & SCE_CTRL_UP) b |= BTN_UP;
    if (pad.buttons & SCE_CTRL_DOWN) b |= BTN_DOWN;
    if (pad.buttons & SCE_CTRL_LEFT) b |= BTN_LEFT;
    if (pad.buttons & SCE_CTRL_RIGHT) b |= BTN_RIGHT;
    if (!(b & (BTN_UP | BTN_DOWN | BTN_LEFT | BTN_RIGHT))) b |= stick(pad.lx, pad.ly);
    if (pad.buttons & SCE_CTRL_CROSS) b |= BTN_OK;
    if (pad.buttons & SCE_CTRL_CIRCLE) b |= BTN_CANCEL;
    if (pad.buttons & SCE_CTRL_TRIANGLE) b |= BTN_MENU;
    if (pad.buttons & SCE_CTRL_SQUARE) b |= BTN_DASH;
    if (pad.buttons & (SCE_CTRL_L1 | SCE_CTRL_L2)) b |= BTN_PAGEUP;
    if (pad.buttons & (SCE_CTRL_R1 | SCE_CTRL_R2)) b |= BTN_PAGEDOWN;
    if (pad.buttons & SCE_CTRL_START) b |= BTN_START;
    if (pad.buttons & SCE_CTRL_SELECT) b |= BTN_SELECT;
    b |= testing::buttons(g_frame);
    g_buttons = b;
}

uint32_t buttons() { return g_buttons; }
uint64_t frame_count() { return g_frame; }

double time_seconds() { return (double)now_us() / 1000000.0; }

static bool scripted() { return g_opt.max_frames > 0 || !g_opt.input_script.empty() || !g_opt.shot_frames.empty(); }

int pending_ticks() {
    if (scripted()) return 1;
    int64_t target = (now_us() - g_start_us) * 60 / 1000000;
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

bool make_dir(const std::string& path) { return sceIoMkdir(path.c_str(), 0777) >= 0; }

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

size_t heap_limit() { return _newlib_heap_size_user; }

void io_begin() {}
void io_end() {}

bool consume_resume() {
    if (!g_resumed) return false;
    g_resumed = false;
    return true;
}

bool debug_warp(int& map, int& x, int& y, int& d) { return testing::warp_due(g_frame, map, x, y, d); }

bool circle_confirms() {
    int v = SCE_SYSTEM_PARAM_ENTER_BUTTON_CROSS;
    return sceAppUtilSystemParamGetInt(SCE_SYSTEM_PARAM_ID_ENTER_BUTTON, &v) == 0 && v == SCE_SYSTEM_PARAM_ENTER_BUTTON_CIRCLE;
}

struct Mutex {
    SceUID sema;
};

static int thread_entry(SceSize size, void* args) {
    ThreadFn fn = nullptr;
    if (size == sizeof(fn)) std::memcpy(&fn, args, sizeof(fn));
    if (fn) fn();
    return 0;
}

bool start_thread(ThreadFn fn) {
    SceUID th = sceKernelCreateThread("worker", thread_entry, 0x10000100 + 8, 0x40000, 0, SCE_KERNEL_CPU_MASK_USER_1 | SCE_KERNEL_CPU_MASK_USER_2, nullptr);
    if (th < 0) return false;
    return sceKernelStartThread(th, sizeof(fn), &fn) >= 0;
}

Mutex* mutex_create() {
    Mutex* m = new Mutex();
    m->sema = sceKernelCreateSema("mutex", 0, 1, 1, nullptr);
    return m;
}

void mutex_lock(Mutex* m) { sceKernelWaitSema(m->sema, 1, nullptr); }
void mutex_unlock(Mutex* m) { sceKernelSignalSema(m->sema, 1); }
void sleep_ms(int ms) { sceKernelDelayThread((SceUInt)ms * 1000); }

}

namespace audio_out {

static MixFn g_fn = nullptr;
static int g_port = -1;
static SceUID g_thread = -1;
static SceUID g_sema = -1;
static volatile bool g_run = false;
static int16_t __attribute__((aligned(64))) g_buf[2][BLOCK * 2];

static int audio_thread(SceSize, void*) {
    int cur = 0;
    while (g_run) {
        lock();
        if (g_fn) g_fn(g_buf[cur], BLOCK);
        else std::memset(g_buf[cur], 0, sizeof(g_buf[cur]));
        unlock();
        sceAudioOutOutput(g_port, g_buf[cur]);
        cur ^= 1;
    }
    return 0;
}

bool start(MixFn fn) {
    g_fn = fn;
    if (g_sema < 0) g_sema = sceKernelCreateSema("audio", 0, 1, 1, nullptr);
    g_port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_BGM, BLOCK, RATE, SCE_AUDIO_OUT_MODE_STEREO);
    if (g_port < 0) return false;
    int vol[2] = {SCE_AUDIO_VOLUME_0DB, SCE_AUDIO_VOLUME_0DB};
    sceAudioOutSetVolume(g_port, (SceAudioOutChannelFlag)(SCE_AUDIO_VOLUME_FLAG_L_CH | SCE_AUDIO_VOLUME_FLAG_R_CH), vol);
    g_thread = sceKernelCreateThread("audio", audio_thread, 0x10000100 - 16, 0x40000, 0, SCE_KERNEL_CPU_MASK_USER_2, nullptr);
    if (g_thread < 0) {
        sceAudioOutReleasePort(g_port);
        g_port = -1;
        return false;
    }
    g_run = true;
    if (sceKernelStartThread(g_thread, 0, nullptr) < 0) {
        g_run = false;
        sceKernelDeleteThread(g_thread);
        sceAudioOutReleasePort(g_port);
        g_port = -1;
        return false;
    }
    return true;
}

void stop() {
    if (!g_run) return;
    g_run = false;
    sceKernelWaitThreadEnd(g_thread, nullptr, nullptr);
    sceKernelDeleteThread(g_thread);
    sceAudioOutReleasePort(g_port);
    g_port = -1;
}

void lock() {
    if (g_sema >= 0) sceKernelWaitSema(g_sema, 1, nullptr);
}

void unlock() {
    if (g_sema >= 0) sceKernelSignalSema(g_sema, 1);
}

}
