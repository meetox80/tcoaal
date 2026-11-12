#include "../audio_out.h"
#include "../gfx.h"
#include "../platform.h"
#include "../common/testing.h"
#include "../../core/fs.h"

#include <SDL.h>
#include <sys/stat.h>

#ifdef __GLIBC__
#include <malloc.h>
#endif

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <fstream>
#include <mutex>
#include <thread>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

namespace plat {

static Options g_opt;
static bool g_running = true;
static uint32_t g_buttons = 0;
static uint64_t g_frame = 0;
static SDL_Window* g_window = nullptr;
static SDL_Renderer* g_renderer = nullptr;
static SDL_Texture* g_screen = nullptr;
static SDL_Texture* g_panel = nullptr;
static uint32_t g_extra = 0;
static bool g_mouse_down = false;
static int g_mouse_x = 0, g_mouse_y = 0;
static bool g_touching = false;
static int g_touch_x = 0, g_touch_y = 0;

constexpr int VIEW_H = gfx::SCREEN_H + gfx::PANEL_H;
constexpr int PANEL_X = (gfx::SCREEN_W - gfx::PANEL_W) / 2;
static SDL_GameController* g_pad = nullptr;
static uint64_t g_next_tick = 0;

bool init(int argc, char** argv) {
    g_opt.data_root = "data";
    g_opt.save_root = "saves";
    std::vector<std::string> args(argv + 1, argv + argc);
    testing::parse_args(g_opt, args);
    if (g_opt.shot_prefix.empty()) g_opt.shot_prefix = "shot";
    testing::setup(g_opt);
    fs::init_data(g_opt.data_root);
    Uint32 flags = SDL_INIT_TIMER;
    if (!g_opt.headless) flags |= SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER | SDL_INIT_AUDIO;
    if (g_opt.headless && std::getenv("TCOAAL_AUDIO")) flags |= SDL_INIT_AUDIO;
    if (SDL_Init(flags) != 0) {
        std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return false;
    }
    if (!g_opt.headless) {
        g_window = SDL_CreateWindow("tcoaal", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, gfx::SCREEN_W * g_opt.scale, VIEW_H * g_opt.scale,
                                    SDL_WINDOW_RESIZABLE);
        g_renderer = SDL_CreateRenderer(g_window, -1, SDL_RENDERER_ACCELERATED | (g_opt.fast ? 0 : SDL_RENDERER_PRESENTVSYNC));
        SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");
        SDL_RenderSetLogicalSize(g_renderer, gfx::SCREEN_W, VIEW_H);
        g_screen = SDL_CreateTexture(g_renderer, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STREAMING, gfx::SCREEN_W, gfx::SCREEN_H);
        if (profile::PANEL) g_panel = SDL_CreateTexture(g_renderer, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STREAMING, gfx::PANEL_W, gfx::PANEL_H);
        for (int i = 0; i < SDL_NumJoysticks(); i++) {
            if (SDL_IsGameController(i)) {
                g_pad = SDL_GameControllerOpen(i);
                break;
            }
        }
    }
    make_dir(g_opt.save_root);
    gfx::init();
    g_next_tick = SDL_GetPerformanceCounter();
    return true;
}

void shutdown() {
    audio_out::stop();
    gfx::shutdown();
    if (g_pad) SDL_GameControllerClose(g_pad);
    if (g_screen) SDL_DestroyTexture(g_screen);
    if (g_panel) SDL_DestroyTexture(g_panel);
    if (g_renderer) SDL_DestroyRenderer(g_renderer);
    if (g_window) SDL_DestroyWindow(g_window);
    SDL_Quit();
}

bool running() { return g_running; }
void request_exit() { g_running = false; }

static uint32_t stick(int x, int y) {
    if (!stick_enabled()) return 0;
    if (x * x + y * y < 16384 * 16384) return 0;
    uint32_t b = 0;
    if (std::abs(x) * 5 > std::abs(y) * 2) b |= x < 0 ? BTN_LEFT : BTN_RIGHT;
    if (std::abs(y) * 5 > std::abs(x) * 2) b |= y < 0 ? BTN_UP : BTN_DOWN;
    return b;
}

void poll() {
    uint32_t b = 0;
    if (!g_opt.headless) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) g_running = false;
            if (e.type == SDL_CONTROLLERDEVICEADDED && !g_pad) g_pad = SDL_GameControllerOpen(e.cdevice.which);
            if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_LEFT) g_mouse_down = true;
            if (e.type == SDL_MOUSEBUTTONUP && e.button.button == SDL_BUTTON_LEFT) g_mouse_down = false;
            if (e.type == SDL_MOUSEBUTTONDOWN || e.type == SDL_MOUSEBUTTONUP) {
                g_mouse_x = e.button.x;
                g_mouse_y = e.button.y;
            }
            if (e.type == SDL_MOUSEMOTION) {
                g_mouse_x = e.motion.x;
                g_mouse_y = e.motion.y;
            }
        }
        const Uint8* k = SDL_GetKeyboardState(nullptr);
        if (k[SDL_SCANCODE_UP]) b |= BTN_UP;
        if (k[SDL_SCANCODE_DOWN]) b |= BTN_DOWN;
        if (k[SDL_SCANCODE_LEFT]) b |= BTN_LEFT;
        if (k[SDL_SCANCODE_RIGHT]) b |= BTN_RIGHT;
        if (k[SDL_SCANCODE_Z] || k[SDL_SCANCODE_RETURN] || k[SDL_SCANCODE_SPACE]) b |= BTN_OK;
        if (k[SDL_SCANCODE_X] || k[SDL_SCANCODE_ESCAPE] || k[SDL_SCANCODE_BACKSPACE]) b |= BTN_CANCEL;
        if (k[SDL_SCANCODE_A] || k[SDL_SCANCODE_TAB]) b |= BTN_MENU;
        if (k[SDL_SCANCODE_LSHIFT] || k[SDL_SCANCODE_RSHIFT]) b |= BTN_DASH;
        if (k[SDL_SCANCODE_Q] || k[SDL_SCANCODE_PAGEUP]) b |= BTN_PAGEUP;
        if (k[SDL_SCANCODE_W] || k[SDL_SCANCODE_PAGEDOWN]) b |= BTN_PAGEDOWN;
        if (k[SDL_SCANCODE_S]) b |= BTN_START;
        if (k[SDL_SCANCODE_D]) b |= BTN_SELECT;
        if (g_pad) {
            auto pb = [&](SDL_GameControllerButton btn) { return SDL_GameControllerGetButton(g_pad, btn) != 0; };
            if (pb(SDL_CONTROLLER_BUTTON_DPAD_UP)) b |= BTN_UP;
            if (pb(SDL_CONTROLLER_BUTTON_DPAD_DOWN)) b |= BTN_DOWN;
            if (pb(SDL_CONTROLLER_BUTTON_DPAD_LEFT)) b |= BTN_LEFT;
            if (pb(SDL_CONTROLLER_BUTTON_DPAD_RIGHT)) b |= BTN_RIGHT;
            if (!(b & (BTN_UP | BTN_DOWN | BTN_LEFT | BTN_RIGHT))) b |= stick(SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_LEFTX), SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_LEFTY));
            if (pb(SDL_CONTROLLER_BUTTON_A)) b |= BTN_OK;
            if (pb(SDL_CONTROLLER_BUTTON_B)) b |= BTN_CANCEL;
            if (pb(SDL_CONTROLLER_BUTTON_Y)) b |= BTN_MENU;
            if (pb(SDL_CONTROLLER_BUTTON_X)) b |= BTN_DASH;
            if (pb(SDL_CONTROLLER_BUTTON_LEFTSHOULDER)) b |= BTN_PAGEUP;
            if (pb(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER)) b |= BTN_PAGEDOWN;
            if (pb(SDL_CONTROLLER_BUTTON_START)) b |= BTN_START;
            if (pb(SDL_CONTROLLER_BUTTON_BACK)) b |= BTN_SELECT;
        }
    }
    b |= testing::buttons(g_frame);
    g_buttons = b;
    int px = g_mouse_x - PANEL_X, py = g_mouse_y - gfx::SCREEN_H;
    g_touching = g_mouse_down && px >= 0 && py >= 0 && px < gfx::PANEL_W && py < gfx::PANEL_H;
    g_touch_x = px;
    g_touch_y = py;
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

double time_seconds() { return (double)SDL_GetPerformanceCounter() / (double)SDL_GetPerformanceFrequency(); }

static void write_png(const std::string& path, const uint32_t* src, int w, int h) {
    std::vector<uint32_t> px(src, src + (size_t)w * h);
    for (auto& p : px) p |= 0xFF000000u;
    stbi_write_png(path.c_str(), w, h, 4, px.data(), w * 4);
    std::printf("shot %s\n", path.c_str());
    std::fflush(stdout);
}

static void write_shot(uint64_t frame) {
    write_png(testing::shot_path(g_opt, frame, "png"), gfx::framebuffer(), gfx::SCREEN_W, gfx::SCREEN_H);
    if (profile::PANEL) write_png(testing::shot_path(g_opt, frame, "panel.png"), gfx::panel_framebuffer(), gfx::PANEL_W, gfx::PANEL_H);
}

int pending_ticks() { return 1; }

void frame_end() {
    gfx::end_frame();
    if (testing::shot_due(g_frame)) write_shot(g_frame);
    if (g_renderer) {
        SDL_UpdateTexture(g_screen, nullptr, gfx::framebuffer(), gfx::SCREEN_W * 4);
        if (g_panel) SDL_UpdateTexture(g_panel, nullptr, gfx::panel_framebuffer(), gfx::PANEL_W * 4);
        SDL_RenderClear(g_renderer);
        SDL_Rect top = {0, 0, gfx::SCREEN_W, gfx::SCREEN_H};
        SDL_Rect bottom = {PANEL_X, gfx::SCREEN_H, gfx::PANEL_W, gfx::PANEL_H};
        SDL_RenderCopy(g_renderer, g_screen, nullptr, &top);
        if (g_panel) SDL_RenderCopy(g_renderer, g_panel, nullptr, &bottom);
        SDL_RenderPresent(g_renderer);
    }
    if (!g_opt.fast) {
        uint64_t freq = SDL_GetPerformanceFrequency();
        g_next_tick += freq / 60;
        uint64_t now = SDL_GetPerformanceCounter();
        if (now < g_next_tick) {
            SDL_Delay((Uint32)((g_next_tick - now) * 1000 / freq));
        } else if (now - g_next_tick > freq / 4) {
            g_next_tick = now;
        }
    }
    g_frame++;
    if (g_opt.max_frames > 0 && g_frame >= (uint64_t)g_opt.max_frames) g_running = false;
    if (testing::exit_due(g_frame)) g_running = false;
}

const Options& options() { return g_opt; }
const std::string& data_root() { return g_opt.data_root; }
const std::string& save_root() { return g_opt.save_root; }

bool make_dir(const std::string& path) { return mkdir(path.c_str(), 0755) == 0; }

void log(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(stderr, fmt, ap);
    va_end(ap);
    std::fputc('\n', stderr);
}

size_t heap_used() {
#ifdef __GLIBC__
    struct mallinfo2 mi = mallinfo2();
    return mi.uordblks + mi.hblkhd;
#else
    return 0;
#endif
}

size_t heap_limit() { return (size_t)g_opt.heap_limit_mb << 20; }

void io_begin() {}
void io_end() {}

bool debug_warp(int& map, int& x, int& y, int& d) { return testing::warp_due(g_frame, map, x, y, d); }

bool consume_resume() { return false; }

bool circle_confirms() { return false; }

struct Mutex {
    std::mutex m;
};

bool start_thread(ThreadFn fn) {
    std::thread(fn).detach();
    return true;
}

Mutex* mutex_create() { return new Mutex(); }
void mutex_lock(Mutex* m) { m->m.lock(); }
void mutex_unlock(Mutex* m) { m->m.unlock(); }
void sleep_ms(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

}

namespace audio_out {

static SDL_AudioDeviceID g_dev = 0;
static MixFn g_fn = nullptr;
static bool g_available = false;

static void callback(void*, Uint8* stream, int len) {
    if (g_fn) g_fn((int16_t*)stream, len / 4);
    else std::memset(stream, 0, len);
}

bool start(MixFn fn) {
    g_fn = fn;
    if (plat::options().headless && !std::getenv("TCOAAL_AUDIO")) {
        g_available = true;
        return true;
    }
    SDL_AudioSpec want{}, have{};
    want.freq = RATE;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = BLOCK;
    want.callback = callback;
    g_dev = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
    if (!g_dev) return false;
    SDL_PauseAudioDevice(g_dev, 0);
    g_available = true;
    return true;
}

bool available() { return g_available; }

void stop() {
    if (g_dev) SDL_CloseAudioDevice(g_dev);
    g_dev = 0;
}

void lock() {
    if (g_dev) SDL_LockAudioDevice(g_dev);
}

void unlock() {
    if (g_dev) SDL_UnlockAudioDevice(g_dev);
}

}
