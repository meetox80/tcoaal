#pragma once

#include <cstddef>

#include "../layout.h"

namespace profile {

constexpr int SCREEN_W = 1280;
constexpr int SCREEN_H = 720;
constexpr float SCALE = 1.0f;
constexpr float OFFSET_X = 0.0f;
constexpr float OFFSET_Y = 0.0f;
constexpr bool PANEL = false;
constexpr int PANEL_W = 0;
constexpr int PANEL_H = 0;
constexpr bool SNAP_WINDOW_SKIN = false;
constexpr TexLayout TEX_LAYOUT = TexLayout::Linear;
constexpr int AUDIO_RATE = 48000;
constexpr size_t IMAGE_BUDGET = 160u << 20;
constexpr int WINDOW_SCALE = 1;
constexpr int SAVE_SLOTS = 50;
constexpr bool CONFIRM_SWAP = false;
constexpr const char* STICK_LABEL = "Left Stick";

}
