#include "../audio_out.h"
#include "../gfx.h"
#include "../platform.h"
#include "../common/testing.h"
#include "../../core/fs.h"

#include <3ds.h>
#include <citro3d.h>
#include <malloc.h>
#include <sys/stat.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

u32 __stacksize__ = 1024 * 1024;

namespace plat {

static Options g_opt;
static bool g_running = true;
static uint32_t g_buttons = 0;
static uint32_t g_extra = 0;
static bool g_touching = false;
static int g_touch_x = 0, g_touch_y = 0;
static uint64_t g_frame = 0;
static FILE* g_log = nullptr;
static PrintConsole g_console;
static s32 g_prio = 0x30;
static bool g_vsync_started = false;
static uint32_t g_vlast = 0;

static bool g_romfs = false;

static std::string find_data(const std::string& base) {
    for (const std::string& path : {std::string("romfs:/tcoaal.pak"), base + "/tcoaal.pak"}) {
        if (fs::has_pack(path)) return path;
    }
    return "";
}

static void show_error(const std::string& base, bool outdated) {
    gfx::begin_frame(gfx::rgba(0, 0, 0));
    gfx::end_frame();
    consoleClear();
    if (outdated) {
        std::printf("This build is out of date.\n\n");
        std::printf("It was made by an older version of\n");
        std::printf("the builder. Build it again from your\n");
        std::printf("copy of the game with the current\n");
        std::printf("builder. Your saves are kept.\n\n");
    } else {
        std::printf("Game data not found.\n\n");
        std::printf("This tcoaal.3dsx has no game inside.\n");
        std::printf("Build your own with the builder from\n");
        std::printf("your copy of the game and start that\n");
        std::printf("file instead, or put tcoaal.pak in\n");
        std::printf("%s\n\n", base.c_str());
    }
    std::printf("Press Start to exit.\n");
    while (aptMainLoop()) {
        hidScanInput();
        if (hidKeysDown() & KEY_START) break;
        gspWaitForVBlank();
    }
}

bool init(int, char**) {
    osSetSpeedupEnable(true);
    APT_SetAppCpuTimeLimit(30);
    svcGetThreadPriority(&g_prio, CUR_THREAD_HANDLE);
    std::string base = "sdmc:/3ds/tcoaal";
    g_romfs = R_SUCCEEDED(romfsInit());
    make_dir("sdmc:/3ds");
    make_dir(base);
    g_opt.data_root = find_data(base);
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
    gfx::init();
    bool outdated = !g_opt.data_root.empty() && fs::pack_version(g_opt.data_root) != (int)fs::PACK_VERSION;
    if (g_opt.data_root.empty() || outdated) {
        log(outdated ? "data: %s is out of date" : "data: nothing in %s", outdated ? g_opt.data_root.c_str() : base.c_str());
        consoleInit(GFX_BOTTOM, &g_console);
        show_error(base, outdated);
        gfx::shutdown();
        if (g_romfs) romfsExit();
        if (g_log) std::fclose(g_log);
        return false;
    }
    fs::init_data(g_opt.data_root);
    return true;
}

void shutdown() {
    audio_out::stop();
    gfx::shutdown();
    if (g_romfs) romfsExit();
    if (g_log) std::fclose(g_log);
    g_log = nullptr;
}

bool running() { return g_running; }
void request_exit() { g_running = false; }

void poll() {
    if (!aptMainLoop()) g_running = false;
    hidScanInput();
    u32 k = hidKeysHeld();
    uint32_t b = 0;
    if (k & KEY_DUP) b |= BTN_UP;
    if (k & KEY_DDOWN) b |= BTN_DOWN;
    if (k & KEY_DLEFT) b |= BTN_LEFT;
    if (k & KEY_DRIGHT) b |= BTN_RIGHT;
    if (stick_enabled() && !(b & (BTN_UP | BTN_DOWN | BTN_LEFT | BTN_RIGHT))) {
        if (k & KEY_CPAD_UP) b |= BTN_UP;
        if (k & KEY_CPAD_DOWN) b |= BTN_DOWN;
        if (k & KEY_CPAD_LEFT) b |= BTN_LEFT;
        if (k & KEY_CPAD_RIGHT) b |= BTN_RIGHT;
    }
    if (k & KEY_A) b |= BTN_OK;
    if (k & KEY_B) b |= BTN_CANCEL;
    if (k & KEY_X) b |= BTN_MENU;
    if (k & KEY_Y) b |= BTN_DASH;
    if (k & KEY_L) b |= BTN_PAGEUP;
    if (k & KEY_R) b |= BTN_PAGEDOWN;
    if (k & KEY_START) b |= BTN_START;
    if (k & KEY_SELECT) b |= BTN_SELECT;
    b |= testing::buttons(g_frame);
    g_buttons = b;
    g_touching = false;
    if (k & KEY_TOUCH) {
        touchPosition t;
        hidTouchRead(&t);
        g_touching = true;
        g_touch_x = t.px;
        g_touch_y = t.py;
    }
    int tx, ty;
    if (testing::touch(g_frame, tx, ty)) {
        g_touching = true;
        g_touch_x = tx;
        g_touch_y = ty;
    }
}

uint32_t buttons() { return g_buttons | g_extra; }
void set_extra_buttons(uint32_t b) { g_extra = b; }

bool touch(int& x, int& y) {
    x = g_touch_x;
    y = g_touch_y;
    return g_touching;
}
uint64_t frame_count() { return g_frame; }

double time_seconds() { return (double)svcGetSystemTick() / SYSCLOCK_ARM11; }

static bool scripted() { return g_opt.max_frames > 0 || !g_opt.input_script.empty() || !g_opt.shot_frames.empty(); }

int pending_ticks() {
    uint32_t now = C3D_FrameCounter(0);
    if (!g_vsync_started || scripted()) {
        g_vsync_started = true;
        g_vlast = now;
        return 1;
    }
    int diff = (int)(now - g_vlast);
    int ticks = std::clamp(diff, 1, 4);
    g_vlast = diff > 4 ? now : g_vlast + ticks;
    return ticks;
}

void frame_end() {
    gfx::end_frame();
    if (testing::shot_due(g_frame)) {
        testing::write_bmp(testing::shot_path(g_opt, g_frame, "bmp"), gfx::framebuffer(), gfx::SCREEN_W, gfx::SCREEN_H, gfx::SCREEN_W);
        testing::write_bmp(testing::shot_path(g_opt, g_frame, "panel.bmp"), gfx::panel_framebuffer(), gfx::PANEL_W, gfx::PANEL_H, gfx::PANEL_W);
    }
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

size_t heap_limit() { return envGetHeapSize(); }

void io_begin() {}
void io_end() {}

bool consume_resume() { return false; }

bool debug_warp(int& map, int& x, int& y, int& d) { return testing::warp_due(g_frame, map, x, y, d); }

bool circle_confirms() { return false; }

struct Mutex {
    LightLock lock;
};

static ThreadFn g_thread_fn = nullptr;

static void thread_entry(void*) {
    if (g_thread_fn) g_thread_fn();
}

bool start_thread(ThreadFn fn) {
    g_thread_fn = fn;
    int prio = std::min(0x3F, (int)g_prio + 1);
    bool n3ds = false;
    APT_CheckNew3DS(&n3ds);
    for (int core : {2, 1, -2}) {
        if (core == 2 && !n3ds) continue;
        if (threadCreate(thread_entry, nullptr, 256 * 1024, prio, core, true)) {
            log("thread: worker on core %d", core);
            return true;
        }
    }
    log("thread: could not start worker");
    return false;
}

Mutex* mutex_create() {
    Mutex* m = new Mutex();
    LightLock_Init(&m->lock);
    return m;
}

void mutex_lock(Mutex* m) { LightLock_Lock(&m->lock); }
void mutex_unlock(Mutex* m) { LightLock_Unlock(&m->lock); }
void sleep_ms(int ms) { svcSleepThread((s64)ms * 1000000); }

}

namespace audio_out {

constexpr int WAVEBUFS = 3;

static MixFn g_fn = nullptr;
static LightLock g_lock;
static bool g_lock_ready = false;
static LightEvent g_event;
static Thread g_thread = nullptr;
static volatile bool g_run = false;
static ndspWaveBuf g_wave[WAVEBUFS];
static int16_t* g_pcm = nullptr;
static bool g_available = false;
static uint8_t g_stand_in_firmware[0x1000];

static bool in_emulator() {
    s64 v = 0;
    return R_SUCCEEDED(svcGetSystemInfo(&v, 0x20000, 0)) && v != 0;
}

static void on_dsp_frame(void*) { LightEvent_Signal(&g_event); }

static void audio_thread(void*) {
    while (g_run) {
        for (ndspWaveBuf& w : g_wave) {
            if (w.status != NDSP_WBUF_FREE && w.status != NDSP_WBUF_DONE) continue;
            lock();
            if (g_fn) g_fn(w.data_pcm16, BLOCK);
            else std::memset(w.data_pcm16, 0, BLOCK * 4);
            unlock();
            DSP_FlushDataCache(w.data_pcm16, BLOCK * 4);
            ndspChnWaveBufAdd(0, &w);
        }
        LightEvent_Wait(&g_event);
    }
}

bool start(MixFn fn) {
    g_fn = fn;
    LightLock_Init(&g_lock);
    g_lock_ready = true;
    Result rc = ndspInit();
    if (R_FAILED(rc) && in_emulator()) {
        plat::log("audio: no sdmc:/3ds/dspfirm.cdc, using the emulator's built-in DSP");
        ndspUseComponent(g_stand_in_firmware, sizeof(g_stand_in_firmware), 0xFF, 0xFF);
        rc = ndspInit();
    }
    if (R_FAILED(rc)) {
        plat::log("audio: ndspInit failed, is sdmc:/3ds/dspfirm.cdc missing?");
        return false;
    }
    g_pcm = (int16_t*)linearAlloc(WAVEBUFS * BLOCK * 4);
    if (!g_pcm) {
        ndspExit();
        return false;
    }
    std::memset(g_pcm, 0, WAVEBUFS * BLOCK * 4);
    ndspSetOutputMode(NDSP_OUTPUT_STEREO);
    ndspChnReset(0);
    ndspChnSetInterp(0, NDSP_INTERP_LINEAR);
    ndspChnSetRate(0, RATE);
    ndspChnSetFormat(0, NDSP_FORMAT_STEREO_PCM16);
    float mix[12] = {1.0f, 1.0f};
    ndspChnSetMix(0, mix);
    std::memset(g_wave, 0, sizeof(g_wave));
    for (int i = 0; i < WAVEBUFS; i++) {
        g_wave[i].data_pcm16 = g_pcm + i * BLOCK * 2;
        g_wave[i].nsamples = BLOCK;
    }
    LightEvent_Init(&g_event, RESET_ONESHOT);
    ndspSetCallback(on_dsp_frame, nullptr);
    s32 prio = 0x30;
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    g_run = true;
    g_thread = threadCreate(audio_thread, nullptr, 256 * 1024, std::max(0x18, (int)prio - 1), -2, false);
    if (!g_thread) {
        g_run = false;
        ndspSetCallback(nullptr, nullptr);
        ndspExit();
        linearFree(g_pcm);
        g_pcm = nullptr;
        return false;
    }
    g_available = true;
    return true;
}

bool available() { return g_available; }

void stop() {
    if (!g_run) return;
    g_run = false;
    LightEvent_Signal(&g_event);
    threadJoin(g_thread, U64_MAX);
    threadFree(g_thread);
    g_thread = nullptr;
    ndspSetCallback(nullptr, nullptr);
    ndspChnWaveBufClear(0);
    ndspExit();
    linearFree(g_pcm);
    g_pcm = nullptr;
}

void lock() {
    if (g_lock_ready) LightLock_Lock(&g_lock);
}

void unlock() {
    if (g_lock_ready) LightLock_Unlock(&g_lock);
}

}
