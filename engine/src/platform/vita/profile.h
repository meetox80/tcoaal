#pragma once

#include <cstddef>

#include "../layout.h"

namespace profile {

constexpr int SCREEN_W = 960;
constexpr int SCREEN_H = 544;
constexpr float SCALE = 0.75f;
constexpr float OFFSET_X = 0.0f;
constexpr float OFFSET_Y = 2.0f;
constexpr bool PANEL = false;
constexpr int PANEL_W = 0;
constexpr int PANEL_H = 0;
constexpr bool SNAP_WINDOW_SKIN = false;
constexpr TexLayout TEX_LAYOUT = TexLayout::Linear;
constexpr int AUDIO_RATE = 48000;
constexpr size_t IMAGE_BUDGET = 96u << 20;
constexpr int WINDOW_SCALE = 1;
constexpr int SAVE_SLOTS = 50;
constexpr bool CONFIRM_SWAP = true;
constexpr const char* STICK_LABEL = "Left Stick";

}
