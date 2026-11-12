#pragma once

#include <cstddef>

#include "../layout.h"

namespace profile {

constexpr int SCREEN_W = 480;
constexpr int SCREEN_H = 272;
constexpr float SCALE = 0.375f;
constexpr float OFFSET_X = 0.0f;
constexpr float OFFSET_Y = 1.0f;
constexpr bool PANEL = false;
constexpr int PANEL_W = 0;
constexpr int PANEL_H = 0;
constexpr bool SNAP_WINDOW_SKIN = false;
constexpr TexLayout TEX_LAYOUT = TexLayout::GsAlpha;
constexpr int AUDIO_RATE = 44100;
constexpr size_t IMAGE_BUDGET = 12u << 20;
constexpr int WINDOW_SCALE = 2;
constexpr int SAVE_SLOTS = 50;
constexpr bool CONFIRM_SWAP = true;
constexpr const char* STICK_LABEL = "Left Stick";

}
