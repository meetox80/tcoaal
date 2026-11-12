#pragma once

#include <cstddef>

#include "../layout.h"

namespace profile {

constexpr int SCREEN_W = 400;
constexpr int SCREEN_H = 225;
constexpr float SCALE = 0.3125f;
constexpr float OFFSET_X = 0.0f;
constexpr float OFFSET_Y = 0.0f;
constexpr bool PANEL = true;
constexpr int PANEL_W = 320;
constexpr int PANEL_H = 240;
constexpr bool SNAP_WINDOW_SKIN = true;
constexpr TexLayout TEX_LAYOUT = TexLayout::PicaTiled;
constexpr int AUDIO_RATE = 44100;
constexpr size_t IMAGE_BUDGET = 22u << 20;
constexpr int WINDOW_SCALE = 2;
constexpr int SAVE_SLOTS = 50;
constexpr bool CONFIRM_SWAP = false;
constexpr const char* STICK_LABEL = "Circle Pad";

}
