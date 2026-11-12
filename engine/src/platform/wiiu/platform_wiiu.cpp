#include "../audio_out.h"
#include "../gfx.h"
#include "../platform.h"
#include "../common/testing.h"
#include "../../core/fs.h"

#include <coreinit/cache.h>
#include <coreinit/debug.h>
#include <coreinit/foreground.h>
#include <coreinit/memexpheap.h>
#include <coreinit/memheap.h>
#include <coreinit/memorymap.h>
#include <coreinit/mutex.h>
#include <coreinit/systeminfo.h>
#include <coreinit/thread.h>
#include <coreinit/time.h>
#include <coreinit/title.h>
#include <malloc.h>
#include <padscore/kpad.h>
#include <padscore/wpad.h>
#include <proc_ui/procui.h>
#include <sndcore2/core.h>
#include <sndcore2/voice.h>
#include <sys/stat.h>
#include <sysapp/launch.h>
#include <vpad/input.h>
#include <whb/gfx.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

static size_t g_sbrk_size = 0;

extern "C" {
void __init_wut_sbrk_heap(MEMHeapHandle heap);
void __init_wut_thread();
void __init_wut_defaultheap();

void __preinit_user(MEMHeapHandle*, MEMHeapHandle*, MEMHeapHandle* mem2) {
    g_sbrk_size = MEMGetAllocatableSizeForExpHeapEx(*mem2, 4);
    __init_wut_sbrk_heap(*mem2);
    __init_wut_thread();
    __init_wut_defaultheap();
}
}

namespace {

struct Thread {
    OSThread os;
    void (*fn)();
};

int thread_entry(int, const char** argv) {
    Thread* t = (Thread*)argv;
    if (t->fn) t->fn();
    return 0;
}

void thread_free(OSThread* os, void* stack) {
    std::free(stack);
    std::free(os);
}

bool spawn(void (*fn)(), uint32_t stack_size, int priority, OSThreadAttributes cores, const char* name) {
    Thread* t = (Thread*)memalign(16, sizeof(Thread));
    uint8_t* stack = (uint8_t*)memalign(16, stack_size);
    if (!t || !stack) {
        std::free(t);
        std::free(stack);
        return false;
    }
    std::memset(t, 0, sizeof(Thread));
    t->fn = fn;
    if (!OSCreateThread(&t->os, thread_entry, 0, (char*)t, stack + stack_size, stack_size, priority, cores)) {
        std::free(t);
        std::free(stack);
        return false;
    }
    OSSetThreadName(&t->os, name);
    OSSetThreadDeallocator(&t->os, thread_free);
    OSDetachThread(&t->os);
    OSResumeThread(&t->os);
    return true;
}

}

namespace plat {

static const char* const BASE = "fs:/vol/external01/wiiu/apps/tcoaal";
constexpr uint64_t HBL_TITLE_ID = 0x0005000013374842ull;
constexpr uint32_t DIRS = BTN_UP | BTN_DOWN | BTN_LEFT | BTN_RIGHT;

static const uint32_t GAMEPAD_MAP[][2] = {
    {VPAD_BUTTON_UP, BTN_UP}, {VPAD_BUTTON_DOWN, BTN_DOWN}, {VPAD_BUTTON_LEFT, BTN_LEFT}, {VPAD_BUTTON_RIGHT, BTN_RIGHT},
    {VPAD_BUTTON_A, BTN_OK}, {VPAD_BUTTON_B, BTN_CANCEL}, {VPAD_BUTTON_X, BTN_MENU}, {VPAD_BUTTON_Y, BTN_DASH},
    {VPAD_BUTTON_L | VPAD_BUTTON_ZL, BTN_PAGEUP}, {VPAD_BUTTON_R | VPAD_BUTTON_ZR, BTN_PAGEDOWN}, {VPAD_BUTTON_PLUS, BTN_START},
    {VPAD_BUTTON_MINUS, BTN_SELECT},
};

static const uint32_t PRO_MAP[][2] = {
    {WPAD_PRO_BUTTON_UP, BTN_UP}, {WPAD_PRO_BUTTON_DOWN, BTN_DOWN}, {WPAD_PRO_BUTTON_LEFT, BTN_LEFT}, {WPAD_PRO_BUTTON_RIGHT, BTN_RIGHT},
    {WPAD_PRO_BUTTON_A, BTN_OK}, {WPAD_PRO_BUTTON_B, BTN_CANCEL}, {WPAD_PRO_BUTTON_X, BTN_MENU}, {WPAD_PRO_BUTTON_Y, BTN_DASH},
    {WPAD_PRO_TRIGGER_L | WPAD_PRO_TRIGGER_ZL, BTN_PAGEUP}, {WPAD_PRO_TRIGGER_R | WPAD_PRO_TRIGGER_ZR, BTN_PAGEDOWN}, {WPAD_PRO_BUTTON_PLUS, BTN_START},
    {WPAD_PRO_BUTTON_MINUS, BTN_SELECT},
};

static const uint32_t CLASSIC_MAP[][2] = {
    {WPAD_CLASSIC_BUTTON_UP, BTN_UP}, {WPAD_CLASSIC_BUTTON_DOWN, BTN_DOWN}, {WPAD_CLASSIC_BUTTON_LEFT, BTN_LEFT},
    {WPAD_CLASSIC_BUTTON_RIGHT, BTN_RIGHT}, {WPAD_CLASSIC_BUTTON_A, BTN_OK}, {WPAD_CLASSIC_BUTTON_B, BTN_CANCEL},
    {WPAD_CLASSIC_BUTTON_X, BTN_MENU}, {WPAD_CLASSIC_BUTTON_Y, BTN_DASH}, {WPAD_CLASSIC_BUTTON_L | WPAD_CLASSIC_BUTTON_ZL, BTN_PAGEUP},
    {WPAD_CLASSIC_BUTTON_R | WPAD_CLASSIC_BUTTON_ZR, BTN_PAGEDOWN}, {WPAD_CLASSIC_BUTTON_PLUS, BTN_START}, {WPAD_CLASSIC_BUTTON_MINUS, BTN_SELECT},
};

static const uint32_t REMOTE_MAP[][2] = {
    {WPAD_BUTTON_RIGHT, BTN_UP}, {WPAD_BUTTON_LEFT, BTN_DOWN}, {WPAD_BUTTON_UP, BTN_LEFT}, {WPAD_BUTTON_DOWN, BTN_RIGHT},
    {WPAD_BUTTON_2, BTN_OK}, {WPAD_BUTTON_1, BTN_CANCEL}, {WPAD_BUTTON_A, BTN_PAGEDOWN}, {WPAD_BUTTON_B, BTN_DASH},
    {WPAD_BUTTON_PLUS, BTN_START}, {WPAD_BUTTON_MINUS, BTN_PAGEUP},
};

static Options g_opt;
static volatile bool g_running = true;
static bool g_from_hbl = false;
static bool g_home_exit = false;
static bool g_proc_done = false;
static bool g_resumed = false;
static uint32_t g_buttons = 0;
static uint32_t g_pads[5] = {};
static uint64_t g_frame = 0;
static FILE* g_log = nullptr;
static OSTime g_start = 0;
static int64_t g_ticks_done = 0;

static uint32_t save_done(void*) {
    OSSavesDone_ReadyToRelease();
    return 0;
}

static uint32_t home_denied(void*) {
    g_home_exit = true;
    return 0;
}

static bool proc_update() {
    for (;;) {
        ProcUIStatus s = ProcUIProcessMessages(TRUE);
        if (s == PROCUI_STATUS_IN_FOREGROUND) return !g_home_exit;
        if (s == PROCUI_STATUS_EXITING) {
            ProcUIShutdown();
            g_proc_done = true;
            return false;
        }
        if (s == PROCUI_STATUS_RELEASE_FOREGROUND) {
            ProcUIDrawDoneRelease();
            g_resumed = true;
        }
    }
}

bool init(int, char**) {
    g_from_hbl = OSGetTitleID() == HBL_TITLE_ID;
    if (g_from_hbl) OSEnableHomeButtonMenu(FALSE);
    ProcUIInitEx(save_done, nullptr);
    if (g_from_hbl) ProcUIRegisterCallback(PROCUI_CALLBACK_HOME_BUTTON_DENIED, home_denied, nullptr, 100);
    g_start = OSGetSystemTime();
    VPADInit();
    KPADInit();
    WPADEnableURCC(TRUE);
    std::string base = BASE;
    make_dir("fs:/vol/external01/wiiu");
    make_dir("fs:/vol/external01/wiiu/apps");
    make_dir(base);
    g_opt.data_root = base + "/tcoaal.pak";
    g_opt.save_root = base + "/saves";
    make_dir(g_opt.save_root);
    g_log = std::fopen((base + "/log.txt").c_str(), "w");
    g_opt.shot_prefix = base + "/shot";
    std::vector<std::string> args = testing::read_args_file(base + "/args.txt");
    for (auto& a : args) {
        if (!a.empty() && a[0] != '-' && a.find(':') == std::string::npos && a.find('/') == std::string::npos && a.find('.') != std::string::npos)
            a = base + "/" + a;
    }
    testing::parse_args(g_opt, args);
    testing::setup(g_opt);
    log("data %s, saves %s, heap %u KB", g_opt.data_root.c_str(), g_opt.save_root.c_str(), (unsigned)(g_sbrk_size / 1024));
    fs::init_data(g_opt.data_root);
    gfx::init();
    return true;
}

void shutdown() {
    audio_out::stop();
    if (!g_proc_done) gfx::shutdown();
    if (g_log) std::fclose(g_log);
    g_log = nullptr;
    if (!g_proc_done) {
        if (g_from_hbl) {
            ProcUIShutdown();
        } else {
            SYSLaunchMenu();
            while (proc_update()) {
            }
        }
        g_proc_done = true;
    }
    KPADShutdown();
    WHBGfxShutdown();
    if (g_from_hbl) SYSRelaunchTitle(0, nullptr);
}

bool running() { return g_running; }
void request_exit() { g_running = false; }

template <size_t N>
static uint32_t map_buttons(uint32_t hold, const uint32_t (&table)[N][2]) {
    uint32_t b = 0;
    for (const auto& m : table) {
        if (hold & m[0]) b |= m[1];
    }
    return b;
}

static uint32_t with_stick(uint32_t b, float x, float y) {
    if (!stick_enabled() || (b & DIRS) || x * x + y * y < 0.25f) return b;
    if (std::fabs(x) * 5 > std::fabs(y) * 2) b |= x < 0 ? BTN_LEFT : BTN_RIGHT;
    if (std::fabs(y) * 5 > std::fabs(x) * 2) b |= y > 0 ? BTN_UP : BTN_DOWN;
    return b;
}

static void read_gamepad() {
    VPADStatus st;
    VPADReadError err = VPAD_READ_SUCCESS;
    if (VPADRead(VPAD_CHAN_0, &st, 1, &err) > 0 && err == VPAD_READ_SUCCESS) g_pads[0] = with_stick(map_buttons(st.hold, GAMEPAD_MAP), st.leftStick.x, st.leftStick.y);
    else if (err != VPAD_READ_NO_SAMPLES && err != VPAD_READ_BUSY) g_pads[0] = 0;
}

static void read_remote(int chan) {
    KPADStatus ks;
    KPADError err = KPAD_ERROR_OK;
    uint32_t& b = g_pads[1 + chan];
    if (KPADReadEx((KPADChan)chan, &ks, 1, &err) < 1 || err != KPAD_ERROR_OK) {
        if (err != KPAD_ERROR_OK && err != KPAD_ERROR_NO_SAMPLES && err != KPAD_ERROR_BUSY) b = 0;
        return;
    }
    if (ks.extensionType == WPAD_EXT_PRO_CONTROLLER) b = with_stick(map_buttons(ks.pro.hold, PRO_MAP), ks.pro.leftStick.x, ks.pro.leftStick.y);
    else if (ks.extensionType == WPAD_EXT_CLASSIC || ks.extensionType == WPAD_EXT_MPLUS_CLASSIC)
        b = with_stick(map_buttons(ks.classic.hold, CLASSIC_MAP), ks.classic.leftStick.x, ks.classic.leftStick.y);
    else if (ks.extensionType == WPAD_EXT_NUNCHUK || ks.extensionType == WPAD_EXT_MPLUS_NUNCHUK)
        b = with_stick(map_buttons(ks.hold, REMOTE_MAP), ks.nunchuk.stick.x, ks.nunchuk.stick.y);
    else b = map_buttons(ks.hold, REMOTE_MAP);
}

void poll() {
    if (!proc_update()) {
        g_running = false;
        return;
    }
    read_gamepad();
    for (int i = 0; i < 4; i++) read_remote(i);
    uint32_t b = testing::buttons(g_frame);
    for (uint32_t p : g_pads) b |= p;
    g_buttons = b;
}

uint32_t buttons() { return g_buttons; }
uint64_t frame_count() { return g_frame; }

static int64_t time_us() { return (int64_t)OSTicksToMicroseconds(OSGetSystemTime() - g_start); }

double time_seconds() { return (double)time_us() / 1000000.0; }

static bool scripted() { return g_opt.max_frames > 0 || !g_opt.input_script.empty() || !g_opt.shot_frames.empty(); }

int pending_ticks() {
    if (scripted()) return 1;
    int64_t target = time_us() * 60 / 1000000;
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
    char line[1024];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    OSReport("tcoaal: %s\n", line);
    if (!g_log) return;
    std::fputs(line, g_log);
    std::fputc('\n', g_log);
    std::fflush(g_log);
}

size_t heap_used() {
    struct mallinfo mi = mallinfo();
    return (size_t)mi.uordblks;
}

size_t heap_limit() { return g_sbrk_size; }

void io_begin() {}
void io_end() {}

bool consume_resume() {
    if (!g_resumed) return false;
    g_resumed = false;
    return true;
}

bool debug_warp(int& map, int& x, int& y, int& d) { return testing::warp_due(g_frame, map, x, y, d); }

bool circle_confirms() { return false; }

struct Mutex {
    OSMutex m;
};

bool start_thread(ThreadFn fn) {
    return spawn(fn, 256 * 1024, 20, (OSThreadAttributes)(OS_THREAD_ATTRIB_AFFINITY_CPU0 | OS_THREAD_ATTRIB_AFFINITY_CPU2), "tcoaal worker");
}

Mutex* mutex_create() {
    Mutex* m = new Mutex();
    OSInitMutex(&m->m);
    return m;
}

void mutex_lock(Mutex* m) { OSLockMutex(&m->m); }
void mutex_unlock(Mutex* m) { OSUnlockMutex(&m->m); }
void sleep_ms(int ms) { OSSleepTicks(OSMillisecondsToTicks(ms)); }

}

namespace audio_out {

constexpr uint32_t RING = 16384;
constexpr uint32_t LEAD = 3 * BLOCK;

static MixFn g_fn = nullptr;
static OSMutex g_lock;
static bool g_lock_ready = false;
static volatile bool g_run = false;
static volatile bool g_done = true;
static int16_t* g_ring = nullptr;
static AXVoice* g_voice[2] = {nullptr, nullptr};
static uint32_t g_write = 0;
static int16_t g_mix[BLOCK * 2];

static int16_t* dsp_buffer(size_t bytes) {
    std::vector<void*> rejected;
    int16_t* found = nullptr;
    for (int i = 0; i < 32 && !found; i++) {
        void* p = memalign(64, bytes);
        if (!p) break;
        uint32_t start = OSEffectiveToPhysical((uint32_t)(uintptr_t)p) & 0x1FFFFFFFu;
        if (((start + (uint32_t)bytes) & 0xE0000000u) == 0) found = (int16_t*)p;
        else rejected.push_back(p);
    }
    for (void* p : rejected) std::free(p);
    return found;
}

static void store(uint32_t at, uint32_t n) {
    for (int c = 0; c < 2; c++) {
        int16_t* ch = g_ring + (size_t)c * RING;
        uint32_t first = std::min(n, RING - at);
        DCFlushRange(ch + at, first * 2);
        if (first < n) DCFlushRange(ch, (n - first) * 2);
    }
}

static void feed() {
    AXVoiceOffsets o;
    while (g_run) {
        AXGetVoiceOffsets(g_voice[0], &o);
        uint32_t play = o.currentOffset & (RING - 1);
        uint32_t ahead = (g_write - play) & (RING - 1);
        if (ahead > RING / 2) {
            g_write = (play + BLOCK / 4) & (RING - 1);
            ahead = BLOCK / 4;
        }
        if (ahead >= LEAD) {
            OSSleepTicks(OSMillisecondsToTicks(2));
            continue;
        }
        lock();
        if (g_fn) g_fn(g_mix, BLOCK);
        else std::memset(g_mix, 0, sizeof(g_mix));
        unlock();
        int16_t* l = g_ring;
        int16_t* r = g_ring + RING;
        for (int i = 0; i < BLOCK; i++) {
            uint32_t at = (g_write + (uint32_t)i) & (RING - 1);
            l[at] = g_mix[i * 2];
            r[at] = g_mix[i * 2 + 1];
        }
        store(g_write, BLOCK);
        g_write = (g_write + BLOCK) & (RING - 1);
    }
    g_done = true;
}

bool start(MixFn fn) {
    g_fn = fn;
    OSInitMutex(&g_lock);
    g_lock_ready = true;
    g_ring = dsp_buffer((size_t)RING * 2 * sizeof(int16_t));
    if (!g_ring) {
        plat::log("audio: no DSP-reachable memory for the output ring");
        return false;
    }
    std::memset(g_ring, 0, (size_t)RING * 2 * sizeof(int16_t));
    DCFlushRange(g_ring, (size_t)RING * 2 * sizeof(int16_t));
    if (!AXIsInit()) {
        AXInitParams params = {};
        params.renderer = AX_INIT_RENDERER_48KHZ;
        params.pipeline = AX_INIT_PIPELINE_SINGLE;
        AXInitWithParams(&params);
    }
    for (int c = 0; c < 2; c++) {
        g_voice[c] = AXAcquireVoice(31, nullptr, nullptr);
        if (!g_voice[c]) {
            plat::log("audio: no free AX voice");
            return false;
        }
    }
    AXVoiceBegin(g_voice[0]);
    AXVoiceBegin(g_voice[1]);
    for (int c = 0; c < 2; c++) {
        AXVoice* v = g_voice[c];
        AXSetVoiceType(v, 0);
        AXVoiceVeData ve = {0x8000, 0};
        AXSetVoiceVe(v, &ve);
        AXVoiceDeviceMixData mix[6];
        std::memset(mix, 0, sizeof(mix));
        mix[c].bus[0].volume = 0x8000;
        AXSetVoiceDeviceMix(v, AX_DEVICE_TYPE_TV, 0, mix);
        AXSetVoiceDeviceMix(v, AX_DEVICE_TYPE_DRC, 0, mix);
        AXSetVoiceSrcRatio(v, (float)RATE / (float)AXGetInputSamplesPerSec());
        AXSetVoiceSrcType(v, AX_VOICE_SRC_TYPE_LINEAR);
        AXVoiceOffsets o = {};
        o.dataType = AX_VOICE_FORMAT_LPCM16;
        o.loopingEnabled = AX_VOICE_LOOP_ENABLED;
        o.loopOffset = 0;
        o.endOffset = RING - 1;
        o.currentOffset = 0;
        o.data = g_ring + (size_t)c * RING;
        AXSetVoiceOffsets(v, &o);
    }
    AXSetVoiceState(g_voice[0], AX_VOICE_STATE_PLAYING);
    AXSetVoiceState(g_voice[1], AX_VOICE_STATE_PLAYING);
    AXVoiceEnd(g_voice[1]);
    AXVoiceEnd(g_voice[0]);
    g_write = 0;
    g_run = true;
    g_done = false;
    if (!spawn(feed, 64 * 1024, 10, OS_THREAD_ATTRIB_AFFINITY_CPU0, "tcoaal audio")) {
        g_run = false;
        g_done = true;
        return false;
    }
    return true;
}

void stop() {
    if (g_run) {
        g_run = false;
        for (int i = 0; i < 200 && !g_done; i++) OSSleepTicks(OSMillisecondsToTicks(1));
    }
    for (AXVoice*& v : g_voice) {
        if (!v) continue;
        AXSetVoiceState(v, AX_VOICE_STATE_STOPPED);
        AXFreeVoice(v);
        v = nullptr;
    }
    if (AXIsInit()) AXQuit();
}

void lock() {
    if (g_lock_ready) OSLockMutex(&g_lock);
}

void unlock() {
    if (g_lock_ready) OSUnlockMutex(&g_lock);
}

}
