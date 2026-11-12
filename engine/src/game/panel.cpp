#include "panel.h"

#include <cmath>
#include <cstring>
#include <string>

#include "../core/image.h"
#include "../platform/audio_out.h"
#include "../platform/gfx.h"
#include "../platform/platform.h"
#include "config.h"
#include "data.h"
#include "map.h"
#include "msgui.h"
#include "save.h"
#include "scenes.h"
#include "sound.h"
#include "state.h"
#include "ui_window.h"
#include "world.h"

namespace panel {

namespace {

enum Id { RUN, SKIP, MENU, SAVE, NEXT, COUNT };

struct Button {
    float x, y, w, h;
    float glow;
    float lit;
};

Button g_buttons[COUNT] = {
    {8, 40, 148, 58, 0, 0}, {164, 40, 148, 58, 0, 0}, {8, 106, 148, 58, 0, 0}, {164, 106, 148, 58, 0, 0}, {8, 172, 304, 60, 0, 0},
};

const ui::Rgb TEXT = ui::hex(0xf0e4d4);
const ui::Rgb DIM = ui::hex(0xa89480);

bool g_fast = false;
bool g_was_down = false;
int g_pressed = -1;
int g_hover = -1;
uint32_t g_inject = 0;
int g_inject_frames = 0;
uint64_t g_time = 0;

float us(float px) { return px / images::SCALE; }
float ux(float px) { return (px - images::OFFSET_X) / images::SCALE; }
float uy(float py) { return (py - images::OFFSET_Y) / images::SCALE; }

bool scene_is(const char* name) {
    Scene* s = scenes::current();
    return s && std::strcmp(s->name(), name) == 0;
}

bool in_menu() {
    if (scenes::is_changing()) return false;
    for (const char* n : {"Menu", "Item", "Save", "Load", "Options", "Controls"}) {
        if (scene_is(n)) return true;
    }
    return false;
}

bool map_idle() {
    return !scenes::is_changing() && scene_is("Map") && world::system().menu_enabled && !world::map().is_event_running() && !msgui::is_busy();
}

bool talking() {
    const Message& m = world::message();
    return scene_is("Map") && m.has_text() && !m.is_choice() && !m.is_number_input() && !m.is_item_choice();
}

bool enabled(int id) {
    switch (id) {
        case MENU: return in_menu() || map_idle();
        case SAVE: return map_idle() && world::system().save_enabled && !save::saves_disabled;
        case NEXT: return talking();
        default: return true;
    }
}

bool is_on(int id) {
    if (id == RUN) return config::get().always_dash;
    if (id == SKIP) return g_fast;
    return false;
}

const char* label(int id) {
    switch (id) {
        case RUN: return "Run";
        case SKIP: return "Fast-forward";
        case MENU: return in_menu() ? "Back" : "Menu";
        case SAVE: return "Save";
        default: return "Continue";
    }
}

int hit(int x, int y) {
    for (int i = 0; i < COUNT; i++) {
        const Button& b = g_buttons[i];
        if (x >= b.x && y >= b.y && x < b.x + b.w && y < b.y + b.h) return i;
    }
    return -1;
}

void press(uint32_t mask) {
    g_inject = mask;
    g_inject_frames = 2;
}

void activate(int id) {
    switch (id) {
        case RUN: {
            config::Config& c = config::get();
            c.always_dash = !c.always_dash;
            config::save();
            sound::play_cursor();
            break;
        }
        case SKIP:
            g_fast = !g_fast;
            sound::play_cursor();
            break;
        case MENU: press(in_menu() ? BTN_CANCEL : BTN_MENU); break;
        case SAVE:
            sound::play_ok();
            scenes::push_save();
            break;
        case NEXT: press(BTN_OK); break;
    }
}

class Frame : public ui::Window {
public:
    void paint(const Button& b, int alpha) {
        move(ux(b.x), uy(b.y), us(b.w), us(b.h));
        opacity = alpha;
        back_opacity = 210;
        draw_frame();
        if (b.glow > 0.02f) {
            float m = us(4);
            cursor = {m - padding, m - padding, width - 2 * m, height - 2 * m};
            contents_opacity = (int)(b.glow * alpha);
            active = false;
            draw_cursor();
        }
    }
};

void draw_backdrop() {
    Image* bg = images::get("img/titles1", data::system()["title1Name"].string());
    if (bg && bg->ow > 0 && bg->oh > 0) {
        float sh = (float)bg->oh;
        float sw = sh * gfx::PANEL_W / gfx::PANEL_H;
        if (sw > bg->ow) {
            sw = (float)bg->ow;
            sh = sw * gfx::PANEL_H / gfx::PANEL_W;
        }
        float sx = (bg->ow - sw) * 0.5f * (1.0f + std::sin(g_time * 0.0021f));
        render::blit_screen(bg, sx, (bg->oh - sh) * 0.5f, sw, sh, 0, 0, gfx::PANEL_W, gfx::PANEL_H, 0xFFFFFFFFu, gfx::Blend::Normal, true);
    }
    gfx::fill(0, 0, gfx::PANEL_W, gfx::PANEL_H, render::color_rgba(12, 7, 7, 196), gfx::Blend::Normal);
    for (int i = 0; i < 12; i++) {
        uint32_t c = render::color_rgba(0, 0, 0, (12 - i) * 9);
        gfx::fill(0, i * 2, gfx::PANEL_W, i * 2 + 2, c, gfx::Blend::Normal);
        gfx::fill(0, gfx::PANEL_H - i * 2 - 2, gfx::PANEL_W, gfx::PANEL_H - i * 2, c, gfx::Blend::Normal);
    }
}

void draw_header() {
    std::string title = data::system()["gameTitle"].string();
    if (title.empty()) title = "The Coffin of Andy and Leyley";
    ui::draw_text(title, ux(12), uy(6), us(220), ui::LEFT, TEXT, 255, 32, us(22));
    if (!scene_is("Title") && !scene_is("Boot") && !scene_is("Credits")) {
        ui::draw_text(save::playtime_text(world::system().seconds_played), ux(238), uy(6), us(72), ui::RIGHT, DIM, 255, 32, us(22));
    }
    gfx::fill(10, 31, 310, 32, render::color_rgba(208, 176, 136, 90), gfx::Blend::Normal);
}

void draw_lamp(float x, float y, float lit) {
    if (lit > 0.01f) {
        uint32_t glow = render::color_rgba(246, 160, 70, (int)(60 * lit));
        gfx::fill(x - 4, y - 2, x + 24, y + 10, glow, gfx::Blend::Add);
        gfx::fill(x - 2, y - 4, x + 22, y + 12, glow, gfx::Blend::Add);
    }
    uint32_t c = render::color_rgba((int)(70 + 176 * lit), (int)(58 + 138 * lit), (int)(50 + 60 * lit), 255);
    gfx::fill(x + 1, y, x + 19, y + 8, c, gfx::Blend::Copy);
    gfx::fill(x, y + 1, x + 20, y + 7, c, gfx::Blend::Copy);
}

void draw_button(Frame& frame, int id) {
    const Button& b = g_buttons[id];
    bool on = enabled(id);
    int alpha = on ? 255 : 110;
    frame.paint(b, alpha);
    int text_alpha = on ? 255 : 100;
    if (id == NEXT) {
        if (on) {
            float pulse = 0.5f + 0.5f * std::sin(g_time * 0.12f);
            gfx::fill(b.x + 6, b.y + 6, b.x + b.w - 6, b.y + b.h - 6, render::color_rgba(246, 160, 70, (int)(10 + 22 * pulse)), gfx::Blend::Add);
            float w = ui::text_width(label(id), 40) * images::SCALE;
            float arrow = ui::text_width(">", 40) * images::SCALE;
            float x0 = std::round(b.x + (b.w - (w + 10 + 14 + arrow)) / 2);
            ui::draw_text(label(id), ux(x0), uy(b.y), us(w + 1), ui::LEFT, TEXT, 255, 40, us(b.h));
            for (int k = 0; k < 3; k++) {
                int a = (int)(90 + 165 * (0.5f + 0.5f * std::sin(g_time * 0.15f - k * 0.9f)));
                ui::draw_text(">", ux(x0 + w + 10 + k * 7), uy(b.y), us(8), ui::LEFT, TEXT, a, 40, us(b.h));
            }
        } else {
            const char* hint = audio_out::available() ? "A interact    B cancel    X menu    Y run" : "No sound: sdmc:/3ds/dspfirm.cdc is missing";
            ui::draw_text(hint, ux(b.x), uy(b.y), us(b.w), ui::CENTER, DIM, 210, 30, us(b.h));
        }
        return;
    }
    ui::draw_text(label(id), ux(b.x + 14), uy(b.y), us(b.w - 28), ui::LEFT, on ? TEXT : DIM, text_alpha, 40, us(b.h));
    if (id == RUN || id == SKIP) draw_lamp(b.x + b.w - 34, b.y + b.h / 2 - 4, b.lit);
    else ui::draw_text(">", ux(b.x + 14), uy(b.y), us(b.w - 28), ui::RIGHT, DIM, text_alpha, 40, us(b.h));
}

}

void update() {
    g_time++;
    int x = 0, y = 0;
    bool down = plat::touch(x, y);
    int over = down ? hit(x, y) : -1;
    if (down && !g_was_down) g_pressed = over;
    if (!down && g_was_down && g_pressed >= 0) {
        if (g_hover == g_pressed) {
            if (enabled(g_pressed)) activate(g_pressed);
            else sound::play_buzzer();
        }
        g_pressed = -1;
    }
    if (down) g_hover = over;
    g_was_down = down;
    plat::set_extra_buttons(g_inject_frames > 0 ? g_inject : 0);
    if (g_inject_frames > 0) g_inject_frames--;
    for (int i = 0; i < COUNT; i++) {
        Button& b = g_buttons[i];
        float glow = down && g_pressed == i && over == i && enabled(i) ? 1.0f : 0.0f;
        b.glow += (glow - b.glow) * 0.4f;
        b.lit += ((is_on(i) ? 1.0f : 0.0f) - b.lit) * 0.25f;
    }
}

void draw() {
    static Frame frame;
    gfx::begin_panel(gfx::rgba(14, 10, 10));
    draw_backdrop();
    draw_header();
    for (int i = 0; i < COUNT; i++) draw_button(frame, i);
    gfx::end_panel();
}

bool fast_forward() { return g_fast; }

}
